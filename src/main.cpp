#include <SDL.h>
#include <SDL_ttf.h>

#include <tinyxml2.h>
#include <zip.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{

constexpr int SCREEN_WIDTH = 480;
constexpr int SCREEN_HEIGHT = 272;

constexpr int READER_VISIBLE_LINES = 11;
constexpr int READER_LINE_HEIGHT = 18;

constexpr const char* BOOKS_PATH = "ms0:/BOOKS";

constexpr zip_uint64_t MAX_XML_SIZE =
    4 * 1024 * 1024;

enum class Screen
{
    Library,
    Reader
};

struct CachedText
{
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
};

struct Book
{
    std::string filename;
    std::string path;

    std::string title;
    std::string author;

    std::string opfPath;
    std::string error;

    bool valid = false;
};

struct ManifestItem
{
    std::string id;
    std::string href;
    std::string mediaType;
};

struct ReaderDocument
{
    std::string title;
    std::string chapterPath;
    std::string text;
    std::string error;

    bool valid = false;
};

struct App
{
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;

    TTF_Font* fontNormal = nullptr;
    TTF_Font* fontSmall = nullptr;
    TTF_Font* fontReader = nullptr;

    SDL_GameController* controller = nullptr;

    std::vector<Book> books;

    std::unordered_map<std::string, CachedText>
        textCache;

    Screen screen = Screen::Library;

    int selected = 0;

    std::string readerTitle;
    std::string readerChapterPath;

    std::vector<std::string> readerLines;

    int readerTopLine = 0;

    bool running = true;
};

bool endsWithEpub(
    const std::string& filename)
{
    if (filename.length() < 5)
        return false;

    std::string extension =
        filename.substr(
            filename.length() - 5
        );

    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char c)
        {
            return static_cast<char>(
                std::tolower(c)
            );
        }
    );

    return extension == ".epub";
}

const char* localName(
    const char* name)
{
    if (!name)
        return "";

    const char* colon =
        std::strchr(name, ':');

    return colon
        ? colon + 1
        : name;
}

tinyxml2::XMLElement*
findElementByLocalName(
    tinyxml2::XMLNode* node,
    const char* wanted)
{
    if (!node)
        return nullptr;

    for (
        tinyxml2::XMLNode* child =
            node->FirstChild();

        child;

        child = child->NextSibling()
    )
    {
        tinyxml2::XMLElement* element =
            child->ToElement();

        if (element)
        {
            if (
                std::strcmp(
                    localName(
                        element->Name()
                    ),
                    wanted
                ) == 0
            )
            {
                return element;
            }
        }

        tinyxml2::XMLElement* nested =
            findElementByLocalName(
                child,
                wanted
            );

        if (nested)
            return nested;
    }

    return nullptr;
}

std::string elementText(
    tinyxml2::XMLElement* element)
{
    if (!element)
        return {};

    const char* text =
        element->GetText();

    return text
        ? std::string(text)
        : std::string();
}

bool readZipEntry(
    zip_t* archive,
    const std::string& entryName,
    std::string& output,
    std::string& error)
{
    zip_stat_t statInfo;

    zip_stat_init(
        &statInfo
    );

    if (
        zip_stat(
            archive,
            entryName.c_str(),
            0,
            &statInfo
        ) != 0
    )
    {
        error =
            "Missing ZIP entry: "
            + entryName;

        return false;
    }

    if (statInfo.size > MAX_XML_SIZE)
    {
        error =
            "XML entry too large: "
            + entryName;

        return false;
    }

    zip_file_t* file =
        zip_fopen(
            archive,
            entryName.c_str(),
            0
        );

    if (!file)
    {
        error =
            "Could not open ZIP entry: "
            + entryName;

        return false;
    }

    output.resize(
        static_cast<std::size_t>(
            statInfo.size
        )
    );

    zip_uint64_t totalRead = 0;

    while (
        totalRead < statInfo.size
    )
    {
        const zip_int64_t readNow =
            zip_fread(
                file,
                output.data() + totalRead,
                statInfo.size - totalRead
            );

        if (readNow < 0)
        {
            zip_fclose(file);

            error =
                "Error reading ZIP entry: "
                + entryName;

            return false;
        }

        if (readNow == 0)
            break;

        totalRead +=
            static_cast<zip_uint64_t>(
                readNow
            );
    }

    zip_fclose(file);

    output.resize(
        static_cast<std::size_t>(
            totalRead
        )
    );

    return true;
}

bool parseContainer(
    zip_t* archive,
    std::string& opfPath,
    std::string& error)
{
    std::string xml;

    if (
        !readZipEntry(
            archive,
            "META-INF/container.xml",
            xml,
            error
        )
    )
    {
        return false;
    }

    tinyxml2::XMLDocument document;

    if (
        document.Parse(
            xml.data(),
            xml.size()
        ) != tinyxml2::XML_SUCCESS
    )
    {
        error =
            "Invalid container.xml";

        return false;
    }

    tinyxml2::XMLElement* rootfile =
        findElementByLocalName(
            &document,
            "rootfile"
        );

    if (!rootfile)
    {
        error =
            "container.xml has no rootfile";

        return false;
    }

    const char* fullPath =
        rootfile->Attribute(
            "full-path"
        );

    if (
        !fullPath ||
        fullPath[0] == '\0'
    )
    {
        error =
            "rootfile has no full-path";

        return false;
    }

    opfPath = fullPath;

    return true;
}

bool parsePackageMetadata(
    zip_t* archive,
    const std::string& opfPath,
    Book& book)
{
    std::string xml;
    std::string error;

    if (
        !readZipEntry(
            archive,
            opfPath,
            xml,
            error
        )
    )
    {
        book.error = error;

        return false;
    }

    tinyxml2::XMLDocument document;

    if (
        document.Parse(
            xml.data(),
            xml.size()
        ) != tinyxml2::XML_SUCCESS
    )
    {
        book.error =
            "Invalid OPF XML";

        return false;
    }

    tinyxml2::XMLElement* metadata =
        findElementByLocalName(
            &document,
            "metadata"
        );

    if (!metadata)
    {
        book.error =
            "OPF has no metadata";

        return false;
    }

    tinyxml2::XMLElement* title =
        findElementByLocalName(
            metadata,
            "title"
        );

    tinyxml2::XMLElement* creator =
        findElementByLocalName(
            metadata,
            "creator"
        );

    book.title =
        elementText(title);

    book.author =
        elementText(creator);

    if (book.title.empty())
        book.title = book.filename;

    if (book.author.empty())
        book.author = "Unknown author";

    return true;
}

Book loadBook(
    const std::string& filename)
{
    Book book;

    book.filename = filename;

    book.path =
        std::string(BOOKS_PATH)
        + "/"
        + filename;

    int zipError = 0;

    zip_t* archive =
        zip_open(
            book.path.c_str(),
            ZIP_RDONLY,
            &zipError
        );

    if (!archive)
    {
        char buffer[128] = {};

        zip_error_to_str(
            buffer,
            sizeof(buffer),
            zipError,
            errno
        );

        book.error =
            std::string(
                "Could not open EPUB: "
            )
            + buffer;

        return book;
    }

    std::string error;

    if (
        !parseContainer(
            archive,
            book.opfPath,
            error
        )
    )
    {
        book.error = error;

        zip_close(archive);

        return book;
    }

    if (
        !parsePackageMetadata(
            archive,
            book.opfPath,
            book
        )
    )
    {
        zip_close(archive);

        return book;
    }

    zip_close(archive);

    book.valid = true;

    return book;
}

std::vector<Book> scanBooks()
{
    std::vector<std::string> filenames;

    DIR* directory =
        opendir(BOOKS_PATH);

    if (!directory)
        return {};

    while (
        dirent* entry =
            readdir(directory)
    )
    {
        std::string filename =
            entry->d_name;

        if (
            filename == "." ||
            filename == ".."
        )
        {
            continue;
        }

        if (endsWithEpub(filename))
        {
            filenames.push_back(
                filename
            );
        }
    }

    closedir(directory);

    std::sort(
        filenames.begin(),
        filenames.end()
    );

    std::vector<Book> books;

    books.reserve(
        filenames.size()
    );

    for (
        const std::string& filename :
        filenames
    )
    {
        books.push_back(
            loadBook(filename)
        );
    }

    return books;
}

std::string archiveDirectory(
    const std::string& path)
{
    const std::size_t pos =
        path.find_last_of('/');

    if (pos == std::string::npos)
        return {};

    return path.substr(
        0,
        pos + 1
    );
}

std::string normalizeArchivePath(
    const std::string& path)
{
    std::vector<std::string> parts;

    std::size_t start = 0;

    while (start <= path.size())
    {
        const std::size_t slash =
            path.find('/', start);

        const std::size_t end =
            (
                slash ==
                std::string::npos
            )
            ? path.size()
            : slash;

        const std::string part =
            path.substr(
                start,
                end - start
            );

        if (
            !part.empty() &&
            part != "."
        )
        {
            if (part == "..")
            {
                if (!parts.empty())
                    parts.pop_back();
            }
            else
            {
                parts.push_back(
                    part
                );
            }
        }

        if (
            slash ==
            std::string::npos
        )
        {
            break;
        }

        start = slash + 1;
    }

    std::string result;

    for (
        std::size_t i = 0;
        i < parts.size();
        ++i
    )
    {
        if (i > 0)
            result += '/';

        result += parts[i];
    }

    return result;
}

std::string resolveArchivePath(
    const std::string& opfPath,
    const std::string& href)
{
    std::string cleanHref =
        href;

    const std::size_t fragment =
        cleanHref.find('#');

    if (
        fragment !=
        std::string::npos
    )
    {
        cleanHref.erase(
            fragment
        );
    }

    const std::size_t query =
        cleanHref.find('?');

    if (
        query !=
        std::string::npos
    )
    {
        cleanHref.erase(
            query
        );
    }

    return normalizeArchivePath(
        archiveDirectory(opfPath)
        + cleanHref
    );
}

bool isBlockElement(
    const std::string& name)
{
    return
        name == "p" ||
        name == "div" ||
        name == "section" ||
        name == "article" ||
        name == "blockquote" ||
        name == "li" ||
        name == "h1" ||
        name == "h2" ||
        name == "h3" ||
        name == "h4" ||
        name == "h5" ||
        name == "h6";
}

bool isIgnoredElement(
    const std::string& name)
{
    return
        name == "head" ||
        name == "style" ||
        name == "script" ||
        name == "svg";
}

void appendXhtmlText(
    tinyxml2::XMLNode* node,
    std::string& output)
{
    if (!node)
        return;

    if (
        tinyxml2::XMLText* text =
            node->ToText()
    )
    {
        const char* value =
            text->Value();

        if (value)
            output += value;

        return;
    }

    tinyxml2::XMLElement* element =
        node->ToElement();

    if (element)
    {
        const std::string name =
            localName(
                element->Name()
            );

        if (isIgnoredElement(name))
            return;

        if (name == "br")
        {
            output += '\n';
            return;
        }

        if (
            isBlockElement(name) &&
            !output.empty() &&
            output.back() != '\n'
        )
        {
            output += '\n';
        }
    }

    for (
        tinyxml2::XMLNode* child =
            node->FirstChild();

        child;

        child = child->NextSibling()
    )
    {
        appendXhtmlText(
            child,
            output
        );
    }

    if (element)
    {
        const std::string name =
            localName(
                element->Name()
            );

        if (
            isBlockElement(name) &&
            !output.empty() &&
            output.back() != '\n'
        )
        {
            output += '\n';
        }
    }
}

std::string normalizeExtractedText(
    const std::string& input)
{
    std::string output;

    bool pendingSpace = false;

    for (unsigned char c : input)
    {
        if (
            c == '\n' ||
            c == '\r'
        )
        {
            while (
                !output.empty() &&
                output.back() == ' '
            )
            {
                output.pop_back();
            }

            if (
                !output.empty() &&
                output.back() != '\n'
            )
            {
                output += '\n';
            }

            pendingSpace = false;

            continue;
        }

        if (
            c == ' ' ||
            c == '\t'
        )
        {
            pendingSpace = true;

            continue;
        }

        if (
            pendingSpace &&
            !output.empty() &&
            output.back() != '\n'
        )
        {
            output += ' ';
        }

        pendingSpace = false;

        output +=
            static_cast<char>(c);
    }

    while (
        !output.empty() &&
        (
            output.back() == ' ' ||
            output.back() == '\n'
        )
    )
    {
        output.pop_back();
    }

    return output;
}

std::string extractXhtmlText(
    const std::string& xml)
{
    tinyxml2::XMLDocument document;

    if (
        document.Parse(
            xml.data(),
            xml.size()
        ) != tinyxml2::XML_SUCCESS
    )
    {
        return {};
    }

    tinyxml2::XMLElement* body =
        findElementByLocalName(
            &document,
            "body"
        );

    if (!body)
        return {};

    std::string output;

    appendXhtmlText(
        body,
        output
    );

    return normalizeExtractedText(
        output
    );
}

ReaderDocument loadReaderDocument(
    const Book& book)
{
    ReaderDocument result;

    result.title =
        book.title;

    int zipError = 0;

    zip_t* archive =
        zip_open(
            book.path.c_str(),
            ZIP_RDONLY,
            &zipError
        );

    if (!archive)
    {
        result.error =
            "Could not reopen EPUB";

        return result;
    }

    std::string opfXml;
    std::string error;

    if (
        !readZipEntry(
            archive,
            book.opfPath,
            opfXml,
            error
        )
    )
    {
        result.error = error;

        zip_close(archive);

        return result;
    }

    tinyxml2::XMLDocument opfDocument;

    if (
        opfDocument.Parse(
            opfXml.data(),
            opfXml.size()
        ) != tinyxml2::XML_SUCCESS
    )
    {
        result.error =
            "Invalid OPF";

        zip_close(archive);

        return result;
    }

    tinyxml2::XMLElement* manifest =
        findElementByLocalName(
            &opfDocument,
            "manifest"
        );

    tinyxml2::XMLElement* spine =
        findElementByLocalName(
            &opfDocument,
            "spine"
        );

    if (!manifest || !spine)
    {
        result.error =
            "OPF has no manifest/spine";

        zip_close(archive);

        return result;
    }

    std::unordered_map<
        std::string,
        ManifestItem
    > manifestItems;

    for (
        tinyxml2::XMLElement* item =
            manifest->FirstChildElement();

        item;

        item = item->NextSiblingElement()
    )
    {
        if (
            std::strcmp(
                localName(
                    item->Name()
                ),
                "item"
            ) != 0
        )
        {
            continue;
        }

        const char* id =
            item->Attribute("id");

        const char* href =
            item->Attribute("href");

        const char* mediaType =
            item->Attribute(
                "media-type"
            );

        if (!id || !href)
            continue;

        ManifestItem manifestItem;

        manifestItem.id = id;
        manifestItem.href = href;

        if (mediaType)
        {
            manifestItem.mediaType =
                mediaType;
        }

        manifestItems[
            manifestItem.id
        ] = manifestItem;
    }

    std::string fallbackText;
    std::string fallbackPath;

    for (
        tinyxml2::XMLElement* itemRef =
            spine->FirstChildElement();

        itemRef;

        itemRef =
            itemRef->NextSiblingElement()
    )
    {
        if (
            std::strcmp(
                localName(
                    itemRef->Name()
                ),
                "itemref"
            ) != 0
        )
        {
            continue;
        }

        const char* idref =
            itemRef->Attribute(
                "idref"
            );

        if (!idref)
            continue;

        auto found =
            manifestItems.find(
                idref
            );

        if (
            found ==
            manifestItems.end()
        )
        {
            continue;
        }

        const ManifestItem& item =
            found->second;

        if (
            item.mediaType !=
            "application/xhtml+xml"
        )
        {
            continue;
        }

        const std::string chapterPath =
            resolveArchivePath(
                book.opfPath,
                item.href
            );

        std::string chapterXml;
        std::string chapterError;

        if (
            !readZipEntry(
                archive,
                chapterPath,
                chapterXml,
                chapterError
            )
        )
        {
            continue;
        }

        const std::string text =
            extractXhtmlText(
                chapterXml
            );

        if (text.empty())
            continue;

        if (fallbackText.empty())
        {
            fallbackText = text;
            fallbackPath = chapterPath;
        }

        /*
         * Covers and title pages are generally tiny.
         * Prefer the first substantial spine document.
         */
        if (text.size() >= 400)
        {
            result.text = text;

            result.chapterPath =
                chapterPath;

            break;
        }
    }

    if (result.text.empty())
    {
        result.text =
            fallbackText;

        result.chapterPath =
            fallbackPath;
    }

    zip_close(archive);

    if (result.text.empty())
    {
        result.error =
            "No readable XHTML found";

        return result;
    }

    result.valid = true;

    return result;
}

TTF_Font* loadFont(
    int size)
{
    const char* paths[] =
    {
        "assets/fonts/NotoSans-Regular.ttf",

        "ms0:/PSP/GAME/LarpReader/"
        "assets/fonts/NotoSans-Regular.ttf"
    };

    for (const char* path : paths)
    {
        TTF_Font* font =
            TTF_OpenFont(
                path,
                size
            );

        if (font)
            return font;
    }

    return nullptr;
}

std::vector<std::string> wrapParagraph(
    TTF_Font* font,
    const std::string& paragraph,
    int maxWidth)
{
    std::vector<std::string> lines;

    std::string currentLine;

    std::size_t start = 0;

    while (start < paragraph.size())
    {
        while (
            start < paragraph.size() &&
            paragraph[start] == ' '
        )
        {
            ++start;
        }

        if (start >= paragraph.size())
            break;

        std::size_t end =
            paragraph.find(
                ' ',
                start
            );

        if (
            end ==
            std::string::npos
        )
        {
            end = paragraph.size();
        }

        const std::string word =
            paragraph.substr(
                start,
                end - start
            );

        const std::string candidate =
            currentLine.empty()
                ? word
                : currentLine
                    + " "
                    + word;

        int width = 0;
        int height = 0;

        TTF_SizeUTF8(
            font,
            candidate.c_str(),
            &width,
            &height
        );

        if (
            width <= maxWidth ||
            currentLine.empty()
        )
        {
            currentLine =
                candidate;
        }
        else
        {
            lines.push_back(
                currentLine
            );

            currentLine =
                word;
        }

        start = end + 1;
    }

    if (!currentLine.empty())
    {
        lines.push_back(
            currentLine
        );
    }

    return lines;
}

std::vector<std::string> wrapText(
    TTF_Font* font,
    const std::string& text,
    int maxWidth)
{
    std::vector<std::string> lines;

    std::size_t start = 0;

    while (start <= text.size())
    {
        std::size_t end =
            text.find(
                '\n',
                start
            );

        if (
            end ==
            std::string::npos
        )
        {
            end = text.size();
        }

        const std::string paragraph =
            text.substr(
                start,
                end - start
            );

        if (!paragraph.empty())
        {
            std::vector<std::string>
                paragraphLines =
                    wrapParagraph(
                        font,
                        paragraph,
                        maxWidth
                    );

            lines.insert(
                lines.end(),
                paragraphLines.begin(),
                paragraphLines.end()
            );

            lines.push_back("");
        }

        if (end == text.size())
            break;

        start = end + 1;
    }

    if (
        !lines.empty() &&
        lines.back().empty()
    )
    {
        lines.pop_back();
    }

    return lines;
}

std::string makeTextCacheKey(
    TTF_Font* font,
    const std::string& text,
    SDL_Color color)
{
    return
        std::to_string(
            reinterpret_cast<
                std::uintptr_t
            >(font)
        )
        + "|"
        + std::to_string(color.r)
        + "|"
        + std::to_string(color.g)
        + "|"
        + std::to_string(color.b)
        + "|"
        + std::to_string(color.a)
        + "|"
        + text;
}

CachedText* getCachedText(
    App& app,
    TTF_Font* font,
    const std::string& text,
    SDL_Color color)
{
    const std::string key =
        makeTextCacheKey(
            font,
            text,
            color
        );

    auto existing =
        app.textCache.find(key);

    if (
        existing !=
        app.textCache.end()
    )
    {
        return &existing->second;
    }

    SDL_Surface* surface =
        TTF_RenderUTF8_Blended(
            font,
            text.c_str(),
            color
        );

    if (!surface)
        return nullptr;

    SDL_Texture* texture =
        SDL_CreateTextureFromSurface(
            app.renderer,
            surface
        );

    if (!texture)
    {
        SDL_FreeSurface(
            surface
        );

        return nullptr;
    }

    CachedText cached;

    cached.texture = texture;
    cached.width = surface->w;
    cached.height = surface->h;

    SDL_FreeSurface(
        surface
    );

    auto result =
        app.textCache.emplace(
            key,
            cached
        );

    return &result.first->second;
}

void drawText(
    App& app,
    TTF_Font* font,
    const std::string& text,
    int x,
    int y,
    SDL_Color color)
{
    if (text.empty())
        return;

    CachedText* cached =
        getCachedText(
            app,
            font,
            text,
            color
        );

    if (!cached)
        return;

    SDL_Rect destination
    {
        x,
        y,
        cached->width,
        cached->height
    };

    SDL_RenderCopy(
        app.renderer,
        cached->texture,
        nullptr,
        &destination
    );
}

void clearTextCache(
    App& app)
{
    for (
        auto& entry :
        app.textCache
    )
    {
        if (entry.second.texture)
        {
            SDL_DestroyTexture(
                entry.second.texture
            );
        }
    }

    app.textCache.clear();
}

bool openSelectedBook(
    App& app)
{
    if (app.books.empty())
        return false;

    if (
        app.selected < 0 ||
        app.selected >=
            static_cast<int>(
                app.books.size()
            )
    )
    {
        return false;
    }

    const Book& book =
        app.books[
            app.selected
        ];

    if (!book.valid)
        return false;

    ReaderDocument document =
        loadReaderDocument(
            book
        );

    if (!document.valid)
    {
        std::printf(
            "Reader error: %s\n",
            document.error.c_str()
        );

        return false;
    }

    app.readerTitle =
        document.title;

    app.readerChapterPath =
        document.chapterPath;

    app.readerLines =
        wrapText(
            app.fontReader,
            document.text,
            436
        );

    app.readerTopLine = 0;

    app.screen =
        Screen::Reader;

    clearTextCache(
        app
    );

    std::printf(
        "Opened chapter: %s\n",
        document.chapterPath.c_str()
    );

    std::printf(
        "Wrapped into %zu lines\n",
        app.readerLines.size()
    );

    return true;
}

bool initialize(
    App& app)
{
    if (
        SDL_Init(
            SDL_INIT_VIDEO |
            SDL_INIT_GAMECONTROLLER
        ) != 0
    )
    {
        std::printf(
            "SDL_Init failed: %s\n",
            SDL_GetError()
        );

        return false;
    }

    if (TTF_Init() != 0)
    {
        std::printf(
            "TTF_Init failed: %s\n",
            TTF_GetError()
        );

        return false;
    }

    app.window =
        SDL_CreateWindow(
            "Larp Reader",
            SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED,
            SCREEN_WIDTH,
            SCREEN_HEIGHT,
            0
        );

    if (!app.window)
        return false;

    app.renderer =
        SDL_CreateRenderer(
            app.window,
            -1,
            SDL_RENDERER_ACCELERATED
        );

    if (!app.renderer)
        return false;

    SDL_RenderSetLogicalSize(
        app.renderer,
        SCREEN_WIDTH,
        SCREEN_HEIGHT
    );

    app.fontNormal =
        loadFont(17);

    app.fontSmall =
        loadFont(12);

    app.fontReader =
        loadFont(14);

    if (
        !app.fontNormal ||
        !app.fontSmall ||
        !app.fontReader
    )
    {
        std::printf(
            "Could not load font: %s\n",
            TTF_GetError()
        );

        return false;
    }

    for (
        int i = 0;
        i < SDL_NumJoysticks();
        ++i
    )
    {
        if (!SDL_IsGameController(i))
            continue;

        app.controller =
            SDL_GameControllerOpen(i);

        if (app.controller)
            break;
    }

    app.books =
        scanBooks();

    std::printf(
        "Larp Reader: found %zu EPUB(s)\n",
        app.books.size()
    );

    return true;
}

void handleLibraryButton(
    App& app,
    Uint8 button)
{
    switch (button)
    {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
        {
            if (!app.books.empty())
            {
                --app.selected;

                if (app.selected < 0)
                {
                    app.selected =
                        static_cast<int>(
                            app.books.size()
                        ) - 1;
                }
            }

            break;
        }

        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        {
            if (!app.books.empty())
            {
                ++app.selected;

                if (
                    app.selected >=
                    static_cast<int>(
                        app.books.size()
                    )
                )
                {
                    app.selected = 0;
                }
            }

            break;
        }

        case SDL_CONTROLLER_BUTTON_A:
        {
            openSelectedBook(
                app
            );

            break;
        }

        case SDL_CONTROLLER_BUTTON_START:
        {
            app.running = false;

            break;
        }

        default:
            break;
    }
}

void handleReaderButton(
    App& app,
    Uint8 button)
{
    switch (button)
    {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
        {
            if (app.readerTopLine > 0)
            {
                --app.readerTopLine;

                clearTextCache(
                    app
                );
            }

            break;
        }

        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        {
            const int maxTop =
                std::max(
                    0,
                    static_cast<int>(
                        app.readerLines.size()
                    )
                    - READER_VISIBLE_LINES
                );

            if (
                app.readerTopLine <
                maxTop
            )
            {
                ++app.readerTopLine;

                clearTextCache(
                    app
                );
            }

            break;
        }

        case SDL_CONTROLLER_BUTTON_B:
        {
            app.screen =
                Screen::Library;

            app.readerLines.clear();
            app.readerTitle.clear();
            app.readerChapterPath.clear();

            app.readerTopLine = 0;

            clearTextCache(
                app
            );

            break;
        }

        case SDL_CONTROLLER_BUTTON_START:
        {
            app.running = false;

            break;
        }

        default:
            break;
    }
}

void handleInput(
    App& app)
{
    SDL_Event event;

    while (
        SDL_PollEvent(
            &event
        )
    )
    {
        if (
            event.type ==
            SDL_QUIT
        )
        {
            app.running = false;

            continue;
        }

        if (
            event.type ==
            SDL_CONTROLLERDEVICEADDED
        )
        {
            if (!app.controller)
            {
                app.controller =
                    SDL_GameControllerOpen(
                        event.cdevice.which
                    );
            }

            continue;
        }

        if (
            event.type !=
            SDL_CONTROLLERBUTTONDOWN
        )
        {
            continue;
        }

        if (
            app.screen ==
            Screen::Library
        )
        {
            handleLibraryButton(
                app,
                event.cbutton.button
            );
        }
        else
        {
            handleReaderButton(
                app,
                event.cbutton.button
            );
        }
    }
}

void renderLibrary(
    App& app)
{
    const SDL_Color background =
        {18, 19, 20, 255};

    const SDL_Color panel =
        {31, 33, 35, 255};

    const SDL_Color text =
        {235, 235, 230, 255};

    const SDL_Color muted =
        {150, 150, 145, 255};

    const SDL_Color selectedText =
        {255, 255, 240, 255};

    const SDL_Color errorText =
        {210, 130, 130, 255};

    SDL_SetRenderDrawColor(
        app.renderer,
        background.r,
        background.g,
        background.b,
        background.a
    );

    SDL_RenderClear(
        app.renderer
    );

    SDL_Rect header
    {
        0,
        0,
        SCREEN_WIDTH,
        42
    };

    SDL_SetRenderDrawColor(
        app.renderer,
        panel.r,
        panel.g,
        panel.b,
        panel.a
    );

    SDL_RenderFillRect(
        app.renderer,
        &header
    );

    drawText(
        app,
        app.fontNormal,
        "Larp Reader",
        16,
        10,
        text
    );

    drawText(
        app,
        app.fontSmall,
        "EPUB Library",
        380,
        14,
        muted
    );

    if (app.books.empty())
    {
        drawText(
            app,
            app.fontNormal,
            "No books found",
            165,
            105,
            text
        );

        drawText(
            app,
            app.fontSmall,
            "Put EPUB files in ms0:/BOOKS/",
            137,
            137,
            muted
        );
    }
    else
    {
        constexpr int firstY = 57;
        constexpr int rowHeight = 43;
        constexpr int visibleBooks = 4;

        int firstBook = 0;

        if (
            app.selected >=
            visibleBooks
        )
        {
            firstBook =
                app.selected
                - visibleBooks
                + 1;
        }

        for (
            int row = 0;
            row < visibleBooks;
            ++row
        )
        {
            const int index =
                firstBook + row;

            if (
                index >=
                static_cast<int>(
                    app.books.size()
                )
            )
            {
                break;
            }

            const int y =
                firstY
                + row * rowHeight;

            const Book& book =
                app.books[index];

            const bool isSelected =
                index ==
                app.selected;

            if (isSelected)
            {
                SDL_Rect selection
                {
                    12,
                    y - 4,
                    456,
                    38
                };

                SDL_SetRenderDrawColor(
                    app.renderer,
                    48,
                    50,
                    50,
                    255
                );

                SDL_RenderFillRect(
                    app.renderer,
                    &selection
                );
            }

            const std::string& title =
                book.valid
                    ? book.title
                    : book.filename;

            const std::string subtitle =
                book.valid
                    ? book.author
                    : book.error;

            drawText(
                app,
                app.fontNormal,
                title,
                22,
                y,
                book.valid
                    ? (
                        isSelected
                            ? selectedText
                            : text
                    )
                    : errorText
            );

            drawText(
                app,
                app.fontSmall,
                subtitle,
                22,
                y + 21,
                book.valid
                    ? muted
                    : errorText
            );
        }
    }

    drawText(
        app,
        app.fontSmall,
        "X Open   UP/DOWN Select",
        14,
        249,
        muted
    );

    drawText(
        app,
        app.fontSmall,
        "START Exit",
        405,
        249,
        muted
    );

    SDL_RenderPresent(
        app.renderer
    );
}

void renderReader(
    App& app)
{
    const SDL_Color background =
        {244, 239, 225, 255};

    const SDL_Color header =
        {224, 218, 201, 255};

    const SDL_Color text =
        {36, 34, 30, 255};

    const SDL_Color muted =
        {100, 95, 85, 255};

    SDL_SetRenderDrawColor(
        app.renderer,
        background.r,
        background.g,
        background.b,
        background.a
    );

    SDL_RenderClear(
        app.renderer
    );

    SDL_Rect headerRect
    {
        0,
        0,
        SCREEN_WIDTH,
        36
    };

    SDL_SetRenderDrawColor(
        app.renderer,
        header.r,
        header.g,
        header.b,
        header.a
    );

    SDL_RenderFillRect(
        app.renderer,
        &headerRect
    );

    drawText(
        app,
        app.fontSmall,
        app.readerTitle,
        16,
        10,
        text
    );

    int y = 45;

    for (
        int row = 0;
        row < READER_VISIBLE_LINES;
        ++row
    )
    {
        const int index =
            app.readerTopLine
            + row;

        if (
            index >=
            static_cast<int>(
                app.readerLines.size()
            )
        )
        {
            break;
        }

        drawText(
            app,
            app.fontReader,
            app.readerLines[index],
            22,
            y,
            text
        );

        y +=
            READER_LINE_HEIGHT;
    }

    const int totalLines =
        static_cast<int>(
            app.readerLines.size()
        );

    const int currentLine =
        std::min(
            totalLines,
            app.readerTopLine + 1
        );

    const std::string progress =
        std::to_string(currentLine)
        + "/"
        + std::to_string(totalLines);

    drawText(
        app,
        app.fontSmall,
        "UP/DOWN Scroll   O Back",
        14,
        253,
        muted
    );

    drawText(
        app,
        app.fontSmall,
        progress,
        425,
        253,
        muted
    );

    SDL_RenderPresent(
        app.renderer
    );
}

void render(
    App& app)
{
    if (
        app.screen ==
        Screen::Library
    )
    {
        renderLibrary(
            app
        );
    }
    else
    {
        renderReader(
            app
        );
    }
}

void shutdown(
    App& app)
{
    clearTextCache(
        app
    );

    if (app.controller)
    {
        SDL_GameControllerClose(
            app.controller
        );
    }

    if (app.fontReader)
    {
        TTF_CloseFont(
            app.fontReader
        );
    }

    if (app.fontSmall)
    {
        TTF_CloseFont(
            app.fontSmall
        );
    }

    if (app.fontNormal)
    {
        TTF_CloseFont(
            app.fontNormal
        );
    }

    if (app.renderer)
    {
        SDL_DestroyRenderer(
            app.renderer
        );
    }

    if (app.window)
    {
        SDL_DestroyWindow(
            app.window
        );
    }

    TTF_Quit();
    SDL_Quit();
}

}

int main(
    int argc,
    char* argv[])
{
    (void)argc;
    (void)argv;

    App app;

    if (!initialize(app))
    {
        shutdown(app);

        return 1;
    }

    while (app.running)
    {
        handleInput(app);
        render(app);

        SDL_Delay(16);
    }

    shutdown(app);

    return 0;
}
