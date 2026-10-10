#include <SDL.h>
#include <SDL_ttf.h>

#include <tinyxml2.h>
#include <zip.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

constexpr const char* BOOKS_PATH =
    "ms0:/BOOKS";

constexpr const char* PROGRESS_PATH =
    "ms0:/PSP/GAME/LarpReader/progress.tsv";

constexpr zip_uint64_t MAX_XML_SIZE =
    4 * 1024 * 1024;

enum class Screen
{
    Library,
    Loading,
    Reader
};

enum class LoadingStage
{
    None,
    OpenStructure,
    LoadChapter,
    Layout,
    Error
};

enum class ChapterPosition
{
    Start,
    End,
    Restore
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
    std::string properties;
};

struct ReaderStructure
{
    std::vector<std::string> chapterPaths;

    std::string error;

    bool valid = false;
};

struct ChapterDocument
{
    std::string path;
    std::string text;
    std::string error;

    bool valid = false;

    /*
     * True means that this is a valid spine item
     * but it contains no text that Larp Reader can
     * render yet.
     *
     * Typical example: an SVG/image-only cover.
     */
    bool skippable = false;
};

struct ReadingProgress
{
    int chapterIndex = 0;
    int topLine = 0;
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

    std::unordered_map<std::string, ReadingProgress>
        savedProgress;

    Screen screen = Screen::Library;
    Screen loadingReturnScreen = Screen::Library;

    int selected = 0;

    std::string readerBookFilename;
    std::string readerTitle;

    std::vector<std::string> readerChapterPaths;

    int readerChapterIndex = -1;

    std::string readerCurrentChapterPath;

    std::vector<std::string> readerLines;

    int readerTopLine = 0;

    LoadingStage loadingStage =
        LoadingStage::None;

    int pendingBookIndex = -1;
    int pendingChapterIndex = -1;

    int pendingSearchDirection = 1;

    ChapterPosition pendingChapterPosition =
        ChapterPosition::Start;

    int pendingRestoreTopLine = 0;

    ChapterDocument pendingChapter;

    std::string loadingMessage;
    std::string loadingDetail;

    int loadingWarmupFrames = 0;

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
        std::strchr(
            name,
            ':'
        );

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

        child =
            child->NextSibling()
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

    if (
        statInfo.size >
        MAX_XML_SIZE
    )
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
        totalRead <
        statInfo.size
    )
    {
        const zip_int64_t readNow =
            zip_fread(
                file,
                output.data()
                    + totalRead,
                statInfo.size
                    - totalRead
            );

        if (readNow < 0)
        {
            zip_fclose(
                file
            );

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

    zip_fclose(
        file
    );

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

    opfPath =
        fullPath;

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
        book.error =
            error;

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
        elementText(
            title
        );

    book.author =
        elementText(
            creator
        );

    if (
        book.title.empty()
    )
    {
        book.title =
            book.filename;
    }

    if (
        book.author.empty()
    )
    {
        book.author =
            "Unknown author";
    }

    return true;
}

Book loadBook(
    const std::string& filename)
{
    Book book;

    book.filename =
        filename;

    book.path =
        std::string(
            BOOKS_PATH
        )
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
        book.error =
            error;

        zip_close(
            archive
        );

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
        zip_close(
            archive
        );

        return book;
    }

    zip_close(
        archive
    );

    book.valid =
        true;

    return book;
}

std::vector<Book> scanBooks()
{
    std::vector<std::string>
        filenames;

    DIR* directory =
        opendir(
            BOOKS_PATH
        );

    if (!directory)
        return {};

    while (
        dirent* entry =
            readdir(
                directory
            )
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

        if (
            endsWithEpub(
                filename
            )
        )
        {
            filenames.push_back(
                filename
            );
        }
    }

    closedir(
        directory
    );

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
            loadBook(
                filename
            )
        );
    }

    return books;
}

std::string archiveDirectory(
    const std::string& path)
{
    const std::size_t pos =
        path.find_last_of(
            '/'
        );

    if (
        pos ==
        std::string::npos
    )
    {
        return {};
    }

    return path.substr(
        0,
        pos + 1
    );
}

std::string normalizeArchivePath(
    const std::string& path)
{
    std::vector<std::string>
        parts;

    std::size_t start = 0;

    while (
        start <=
        path.size()
    )
    {
        const std::size_t slash =
            path.find(
                '/',
                start
            );

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
            if (
                part == ".."
            )
            {
                if (
                    !parts.empty()
                )
                {
                    parts.pop_back();
                }
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

        start =
            slash + 1;
    }

    std::string result;

    for (
        std::size_t i = 0;
        i < parts.size();
        ++i
    )
    {
        if (i > 0)
        {
            result += '/';
        }

        result +=
            parts[i];
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
        cleanHref.find(
            '#'
        );

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
        cleanHref.find(
            '?'
        );

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
        archiveDirectory(
            opfPath
        )
        + cleanHref
    );
}

bool propertyContainsToken(
    const std::string& properties,
    const std::string& token)
{
    std::size_t start = 0;

    while (
        start <
        properties.size()
    )
    {
        while (
            start <
                properties.size() &&
            std::isspace(
                static_cast<unsigned char>(
                    properties[start]
                )
            )
        )
        {
            ++start;
        }

        if (
            start >=
            properties.size()
        )
        {
            break;
        }

        std::size_t end =
            start;

        while (
            end <
                properties.size() &&
            !std::isspace(
                static_cast<unsigned char>(
                    properties[end]
                )
            )
        )
        {
            ++end;
        }

        if (
            properties.substr(
                start,
                end - start
            ) == token
        )
        {
            return true;
        }

        start =
            end;
    }

    return false;
}

ReaderStructure loadReaderStructure(
    const Book& book)
{
    ReaderStructure result;

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
        result.error =
            error;

        zip_close(
            archive
        );

        return result;
    }

    zip_close(
        archive
    );

    tinyxml2::XMLDocument document;

    if (
        document.Parse(
            opfXml.data(),
            opfXml.size()
        ) != tinyxml2::XML_SUCCESS
    )
    {
        result.error =
            "Invalid OPF";

        return result;
    }

    tinyxml2::XMLElement* manifest =
        findElementByLocalName(
            &document,
            "manifest"
        );

    tinyxml2::XMLElement* spine =
        findElementByLocalName(
            &document,
            "spine"
        );

    if (
        !manifest ||
        !spine
    )
    {
        result.error =
            "OPF has no manifest/spine";

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

        item =
            item->NextSiblingElement()
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
            item->Attribute(
                "id"
            );

        const char* href =
            item->Attribute(
                "href"
            );

        const char* mediaType =
            item->Attribute(
                "media-type"
            );

        const char* properties =
            item->Attribute(
                "properties"
            );

        if (
            !id ||
            !href
        )
        {
            continue;
        }

        ManifestItem manifestItem;

        manifestItem.id =
            id;

        manifestItem.href =
            href;

        if (mediaType)
        {
            manifestItem.mediaType =
                mediaType;
        }

        if (properties)
        {
            manifestItem.properties =
                properties;
        }

        manifestItems[
            manifestItem.id
        ] =
            manifestItem;
    }

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

        const char* linear =
            itemRef->Attribute(
                "linear"
            );

        if (
            linear &&
            std::strcmp(
                linear,
                "no"
            ) == 0
        )
        {
            continue;
        }

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

        /*
         * EPUB 3 navigation documents are not part
         * of normal reading content for us.
         */
        if (
            propertyContainsToken(
                item.properties,
                "nav"
            )
        )
        {
            continue;
        }

        result.chapterPaths.push_back(
            resolveArchivePath(
                book.opfPath,
                item.href
            )
        );
    }

    if (
        result.chapterPaths.empty()
    )
    {
        result.error =
            "No readable spine items";

        return result;
    }

    result.valid =
        true;

    return result;
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
        {
            output +=
                value;
        }

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

        if (
            isIgnoredElement(
                name
            )
        )
        {
            return;
        }

        if (
            name == "br"
        )
        {
            output += '\n';

            return;
        }

        if (
            isBlockElement(
                name
            ) &&
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

        child =
            child->NextSibling()
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
            isBlockElement(
                name
            ) &&
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

    bool pendingSpace =
        false;

    for (
        unsigned char c :
        input
    )
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

            pendingSpace =
                false;

            continue;
        }

        if (
            c == ' ' ||
            c == '\t'
        )
        {
            pendingSpace =
                true;

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

        pendingSpace =
            false;

        output +=
            static_cast<char>(
                c
            );
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

ChapterDocument loadChapterDocument(
    const Book& book,
    const std::string& chapterPath)
{
    ChapterDocument result;

    result.path =
        chapterPath;

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

    std::string chapterXml;

    if (
        !readZipEntry(
            archive,
            chapterPath,
            chapterXml,
            result.error
        )
    )
    {
        zip_close(
            archive
        );

        /*
         * Failure to actually retrieve the resource is
         * considered an EPUB/file error, not something
         * that should be silently skipped.
         */
        result.skippable =
            false;

        return result;
    }

    zip_close(
        archive
    );

    result.text =
        extractXhtmlText(
            chapterXml
        );

    if (
        result.text.empty()
    )
    {
        /*
         * The XHTML exists and was opened correctly,
         * but contains no textual body that we support.
         *
         * Image/SVG cover pages fall into this case.
         */
        result.error =
            "Spine item contains no readable text";

        result.skippable =
            true;

        return result;
    }

    result.valid =
        true;

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

    for (
        const char* path :
        paths
    )
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

int measureTextWidth(
    TTF_Font* font,
    const std::string& text,
    std::unordered_map<
        std::string,
        int
    >& widthCache)
{
    auto found =
        widthCache.find(
            text
        );

    if (
        found !=
        widthCache.end()
    )
    {
        return found->second;
    }

    int width = 0;
    int height = 0;

    if (
        TTF_SizeUTF8(
            font,
            text.c_str(),
            &width,
            &height
        ) != 0
    )
    {
        width =
            0;
    }

    if (
        widthCache.size() <
        4096
    )
    {
        widthCache[
            text
        ] =
            width;
    }

    return width;
}

std::vector<std::string>
wrapParagraphFast(
    TTF_Font* font,
    const std::string& paragraph,
    int maxWidth,
    int spaceWidth,
    std::unordered_map<
        std::string,
        int
    >& widthCache)
{
    std::vector<std::string>
        lines;

    std::string currentLine;

    int currentWidth =
        0;

    std::size_t start =
        0;

    while (
        start <
        paragraph.size()
    )
    {
        while (
            start <
                paragraph.size() &&
            paragraph[start] == ' '
        )
        {
            ++start;
        }

        if (
            start >=
            paragraph.size()
        )
        {
            break;
        }

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
            end =
                paragraph.size();
        }

        const std::string word =
            paragraph.substr(
                start,
                end - start
            );

        const int wordWidth =
            measureTextWidth(
                font,
                word,
                widthCache
            );

        if (
            currentLine.empty()
        )
        {
            currentLine =
                word;

            currentWidth =
                wordWidth;
        }
        else
        {
            const int candidateWidth =
                currentWidth
                + spaceWidth
                + wordWidth;

            if (
                candidateWidth <=
                maxWidth
            )
            {
                currentLine +=
                    " ";

                currentLine +=
                    word;

                currentWidth =
                    candidateWidth;
            }
            else
            {
                lines.push_back(
                    currentLine
                );

                currentLine =
                    word;

                currentWidth =
                    wordWidth;
            }
        }

        start =
            end + 1;
    }

    if (
        !currentLine.empty()
    )
    {
        lines.push_back(
            currentLine
        );
    }

    return lines;
}

std::vector<std::string>
wrapTextFast(
    TTF_Font* font,
    const std::string& text,
    int maxWidth)
{
    std::vector<std::string>
        lines;

    std::unordered_map<
        std::string,
        int
    > widthCache;

    widthCache.reserve(
        1024
    );

    int spaceWidth =
        0;

    int unusedHeight =
        0;

    TTF_SizeUTF8(
        font,
        " ",
        &spaceWidth,
        &unusedHeight
    );

    std::size_t start =
        0;

    while (
        start <=
        text.size()
    )
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
            end =
                text.size();
        }

        const std::string paragraph =
            text.substr(
                start,
                end - start
            );

        if (
            !paragraph.empty()
        )
        {
            std::vector<std::string>
                paragraphLines =
                    wrapParagraphFast(
                        font,
                        paragraph,
                        maxWidth,
                        spaceWidth,
                        widthCache
                    );

            lines.insert(
                lines.end(),
                paragraphLines.begin(),
                paragraphLines.end()
            );

            lines.push_back(
                ""
            );
        }

        if (
            end ==
            text.size()
        )
        {
            break;
        }

        start =
            end + 1;
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

void loadProgress(
    App& app)
{
    FILE* file =
        std::fopen(
            PROGRESS_PATH,
            "r"
        );

    if (!file)
        return;

    char buffer[1024];

    while (
        std::fgets(
            buffer,
            sizeof(buffer),
            file
        )
    )
    {
        std::string line =
            buffer;

        while (
            !line.empty() &&
            (
                line.back() == '\n' ||
                line.back() == '\r'
            )
        )
        {
            line.pop_back();
        }

        if (
            line.empty()
        )
        {
            continue;
        }

        std::vector<std::string>
            fields;

        std::size_t start =
            0;

        while (
            start <=
            line.size()
        )
        {
            std::size_t tab =
                line.find(
                    '\t',
                    start
                );

            if (
                tab ==
                std::string::npos
            )
            {
                fields.push_back(
                    line.substr(
                        start
                    )
                );

                break;
            }

            fields.push_back(
                line.substr(
                    start,
                    tab - start
                )
            );

            start =
                tab + 1;
        }

        if (
            fields.size() <
            2
        )
        {
            continue;
        }

        ReadingProgress progress;

        /*
         * Compatibility with the old v0.0.5 format:
         *
         * filename<TAB>topLine
         */
        if (
            fields.size() == 2
        )
        {
            progress.chapterIndex =
                0;

            progress.topLine =
                std::atoi(
                    fields[1].c_str()
                );
        }
        else
        {
            progress.chapterIndex =
                std::atoi(
                    fields[1].c_str()
                );

            progress.topLine =
                std::atoi(
                    fields[2].c_str()
                );
        }

        progress.chapterIndex =
            std::max(
                0,
                progress.chapterIndex
            );

        progress.topLine =
            std::max(
                0,
                progress.topLine
            );

        app.savedProgress[
            fields[0]
        ] =
            progress;
    }

    std::fclose(
        file
    );
}

void saveProgress(
    const App& app)
{
    FILE* file =
        std::fopen(
            PROGRESS_PATH,
            "w"
        );

    if (!file)
    {
        std::printf(
            "Could not write progress file: %s\n",
            PROGRESS_PATH
        );

        return;
    }

    for (
        const auto& entry :
        app.savedProgress
    )
    {
        std::fprintf(
            file,
            "%s\t%d\t%d\n",
            entry.first.c_str(),
            entry.second.chapterIndex,
            entry.second.topLine
        );
    }

    std::fclose(
        file
    );
}

void rememberCurrentProgress(
    App& app)
{
    if (
        app.readerBookFilename.empty() ||
        app.readerChapterIndex < 0
    )
    {
        return;
    }

    ReadingProgress progress;

    progress.chapterIndex =
        app.readerChapterIndex;

    progress.topLine =
        app.readerTopLine;

    app.savedProgress[
        app.readerBookFilename
    ] =
        progress;

    saveProgress(
        app
    );
}

int maximumTopLine(
    const App& app)
{
    return std::max(
        0,
        static_cast<int>(
            app.readerLines.size()
        )
        - READER_VISIBLE_LINES
    );
}

int totalPages(
    const App& app)
{
    if (
        app.readerLines.empty()
    )
    {
        return 1;
    }

    return
        (
            static_cast<int>(
                app.readerLines.size()
            )
            + READER_VISIBLE_LINES
            - 1
        )
        /
        READER_VISIBLE_LINES;
}

int currentPage(
    const App& app)
{
    if (
        app.readerLines.empty()
    )
    {
        return 1;
    }

    return std::min(
        (
            app.readerTopLine
            /
            READER_VISIBLE_LINES
        )
        + 1,
        totalPages(
            app
        )
    );
}

void clampReaderPosition(
    App& app)
{
    app.readerTopLine =
        std::clamp(
            app.readerTopLine,
            0,
            maximumTopLine(
                app
            )
        );
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
        + std::to_string(
            color.r
        )
        + "|"
        + std::to_string(
            color.g
        )
        + "|"
        + std::to_string(
            color.b
        )
        + "|"
        + std::to_string(
            color.a
        )
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
        app.textCache.find(
            key
        );

    if (
        existing !=
        app.textCache.end()
    )
    {
        return
            &existing->second;
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

    cached.texture =
        texture;

    cached.width =
        surface->w;

    cached.height =
        surface->h;

    SDL_FreeSurface(
        surface
    );

    auto result =
        app.textCache.emplace(
            key,
            cached
        );

    return
        &result.first->second;
}

void drawText(
    App& app,
    TTF_Font* font,
    const std::string& text,
    int x,
    int y,
    SDL_Color color)
{
    if (
        text.empty()
    )
    {
        return;
    }

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
        if (
            entry.second.texture
        )
        {
            SDL_DestroyTexture(
                entry.second.texture
            );
        }
    }

    app.textCache.clear();
}

void loadingFailed(
    App& app,
    const std::string& error)
{
    app.loadingStage =
        LoadingStage::Error;

    app.loadingMessage =
        "Could not open book";

    app.loadingDetail =
        error;

    app.loadingWarmupFrames =
        0;

    std::printf(
        "Loading failed: %s\n",
        error.c_str()
    );
}

void finishChapterSearchWithoutResult(
    App& app)
{
    /*
     * When navigating from an already-open chapter,
     * reaching a cover or other non-text item at the
     * end of the search is not an error.
     *
     * Simply remain on the chapter the user was
     * already reading.
     */
    if (
        app.loadingReturnScreen ==
            Screen::Reader &&
        !app.readerLines.empty() &&
        app.readerChapterIndex >= 0
    )
    {
        std::printf(
            "No readable spine item in that direction; "
            "staying on chapter %d/%zu\n",
            app.readerChapterIndex + 1,
            app.readerChapterPaths.size()
        );

        app.pendingChapter =
            ChapterDocument{};

        app.pendingChapterIndex =
            -1;

        app.loadingStage =
            LoadingStage::None;

        app.loadingMessage.clear();
        app.loadingDetail.clear();

        app.screen =
            Screen::Reader;

        clearTextCache(
            app
        );

        return;
    }

    /*
     * If we were opening the book for the first time
     * and couldn't find any readable item, then there
     * genuinely is nothing we can currently display.
     */
    loadingFailed(
        app,
        "No readable text chapters found"
    );
}

void scheduleChapterLoad(
    App& app,
    int chapterIndex,
    ChapterPosition position,
    int restoreTopLine,
    int searchDirection,
    Screen returnScreen)
{
    if (
        chapterIndex < 0 ||
        chapterIndex >=
            static_cast<int>(
                app.readerChapterPaths.size()
            )
    )
    {
        return;
    }

    app.pendingChapterIndex =
        chapterIndex;

    app.pendingChapterPosition =
        position;

    app.pendingRestoreTopLine =
        restoreTopLine;

    app.pendingSearchDirection =
        searchDirection >= 0
        ? 1
        : -1;

    app.pendingChapter =
        ChapterDocument{};

    app.loadingStage =
        LoadingStage::LoadChapter;

    app.loadingReturnScreen =
        returnScreen;

    app.loadingMessage =
        "Loading chapter...";

    app.loadingDetail =
        "Chapter "
        + std::to_string(
            chapterIndex + 1
        )
        + "/"
        + std::to_string(
            app.readerChapterPaths.size()
        );

    app.loadingWarmupFrames =
        0;

    app.screen =
        Screen::Loading;

    clearTextCache(
        app
    );
}

void skipCurrentSpineItem(
    App& app,
    const std::string& reason)
{
    if (
        app.pendingChapterIndex < 0 ||
        app.pendingChapterIndex >=
            static_cast<int>(
                app.readerChapterPaths.size()
            )
    )
    {
        finishChapterSearchWithoutResult(
            app
        );

        return;
    }

    std::printf(
        "Skipping non-text spine item %d/%zu: %s (%s)\n",
        app.pendingChapterIndex + 1,
        app.readerChapterPaths.size(),
        app.readerChapterPaths[
            app.pendingChapterIndex
        ].c_str(),
        reason.c_str()
    );

    const int nextIndex =
        app.pendingChapterIndex
        + app.pendingSearchDirection;

    if (
        nextIndex < 0 ||
        nextIndex >=
            static_cast<int>(
                app.readerChapterPaths.size()
            )
    )
    {
        finishChapterSearchWithoutResult(
            app
        );

        return;
    }

    const ChapterPosition nextPosition =
        app.pendingSearchDirection > 0
        ? ChapterPosition::Start
        : ChapterPosition::End;

    scheduleChapterLoad(
        app,
        nextIndex,
        nextPosition,
        0,
        app.pendingSearchDirection,
        app.loadingReturnScreen
    );
}

void beginOpenSelectedBook(
    App& app)
{
    if (
        app.books.empty()
    )
    {
        return;
    }

    if (
        app.selected < 0 ||
        app.selected >=
            static_cast<int>(
                app.books.size()
            )
    )
    {
        return;
    }

    const Book& book =
        app.books[
            app.selected
        ];

    if (!book.valid)
        return;

    app.pendingBookIndex =
        app.selected;

    app.readerBookFilename.clear();
    app.readerTitle.clear();
    app.readerChapterPaths.clear();

    app.readerChapterIndex =
        -1;

    app.readerCurrentChapterPath.clear();
    app.readerLines.clear();

    app.readerTopLine =
        0;

    app.loadingStage =
        LoadingStage::OpenStructure;

    app.loadingReturnScreen =
        Screen::Library;

    app.loadingMessage =
        "Opening EPUB...";

    app.loadingDetail =
        "Reading book structure";

    app.loadingWarmupFrames =
        0;

    app.screen =
        Screen::Loading;

    clearTextCache(
        app
    );
}

void processLoadingStage(
    App& app)
{
    if (
        app.screen !=
        Screen::Loading
    )
    {
        return;
    }

    if (
        app.loadingStage ==
        LoadingStage::Error
    )
    {
        return;
    }

    /*
     * Give SDL a couple of frames to put the loading
     * screen on the framebuffer before blocking work.
     */
    if (
        app.loadingWarmupFrames <
        2
    )
    {
        ++app.loadingWarmupFrames;

        return;
    }

    if (
        app.loadingStage ==
        LoadingStage::OpenStructure
    )
    {
        if (
            app.pendingBookIndex < 0 ||
            app.pendingBookIndex >=
                static_cast<int>(
                    app.books.size()
                )
        )
        {
            loadingFailed(
                app,
                "Invalid book index"
            );

            return;
        }

        const Book& book =
            app.books[
                app.pendingBookIndex
            ];

        const Uint32 startTime =
            SDL_GetTicks();

        ReaderStructure structure =
            loadReaderStructure(
                book
            );

        const Uint32 elapsed =
            SDL_GetTicks()
            - startTime;

        std::printf(
            "Spine parsing took %u ms\n",
            static_cast<unsigned>(
                elapsed
            )
        );

        if (!structure.valid)
        {
            loadingFailed(
                app,
                structure.error
            );

            return;
        }

        app.readerBookFilename =
            book.filename;

        app.readerTitle =
            book.title;

        app.readerChapterPaths =
            std::move(
                structure.chapterPaths
            );

        ReadingProgress progress;

        auto saved =
            app.savedProgress.find(
                book.filename
            );

        if (
            saved !=
            app.savedProgress.end()
        )
        {
            progress =
                saved->second;
        }

        progress.chapterIndex =
            std::clamp(
                progress.chapterIndex,
                0,
                static_cast<int>(
                    app.readerChapterPaths.size()
                ) - 1
            );

        scheduleChapterLoad(
            app,
            progress.chapterIndex,
            ChapterPosition::Restore,
            progress.topLine,
            1,
            Screen::Library
        );

        return;
    }

    if (
        app.loadingStage ==
        LoadingStage::LoadChapter
    )
    {
        if (
            app.pendingBookIndex < 0 ||
            app.pendingBookIndex >=
                static_cast<int>(
                    app.books.size()
                )
        )
        {
            loadingFailed(
                app,
                "Invalid book index"
            );

            return;
        }

        if (
            app.pendingChapterIndex < 0 ||
            app.pendingChapterIndex >=
                static_cast<int>(
                    app.readerChapterPaths.size()
                )
        )
        {
            loadingFailed(
                app,
                "Invalid chapter index"
            );

            return;
        }

        const Book& book =
            app.books[
                app.pendingBookIndex
            ];

        const std::string& chapterPath =
            app.readerChapterPaths[
                app.pendingChapterIndex
            ];

        const Uint32 startTime =
            SDL_GetTicks();

        ChapterDocument chapter =
            loadChapterDocument(
                book,
                chapterPath
            );

        const Uint32 elapsed =
            SDL_GetTicks()
            - startTime;

        std::printf(
            "Chapter load took %u ms: %s\n",
            static_cast<unsigned>(
                elapsed
            ),
            chapterPath.c_str()
        );

        if (!chapter.valid)
        {
            if (
                chapter.skippable
            )
            {
                skipCurrentSpineItem(
                    app,
                    chapter.error
                );

                return;
            }

            /*
             * Missing resource, ZIP failure, etc.
             * Those are real EPUB errors and should
             * still be reported.
             */
            loadingFailed(
                app,
                chapter.error
            );

            return;
        }

        app.pendingChapter =
            std::move(
                chapter
            );

        app.loadingStage =
            LoadingStage::Layout;

        app.loadingMessage =
            "Preparing pages...";

        app.loadingDetail =
            "Chapter "
            + std::to_string(
                app.pendingChapterIndex + 1
            )
            + "/"
            + std::to_string(
                app.readerChapterPaths.size()
            );

        app.loadingWarmupFrames =
            0;

        return;
    }

    if (
        app.loadingStage ==
        LoadingStage::Layout
    )
    {
        const Uint32 startTime =
            SDL_GetTicks();

        std::vector<std::string> lines =
            wrapTextFast(
                app.fontReader,
                app.pendingChapter.text,
                436
            );

        const Uint32 elapsed =
            SDL_GetTicks()
            - startTime;

        std::printf(
            "Chapter layout took %u ms\n",
            static_cast<unsigned>(
                elapsed
            )
        );

        if (
            lines.empty()
        )
        {
            /*
             * This should be rare because text extraction
             * already rejected empty content, but treat it
             * the same way as a visual/non-text spine item.
             */
            skipCurrentSpineItem(
                app,
                "Chapter produced no visible lines"
            );

            return;
        }

        app.readerLines =
            std::move(
                lines
            );

        app.readerChapterIndex =
            app.pendingChapterIndex;

        app.readerCurrentChapterPath =
            app.pendingChapter.path;

        switch (
            app.pendingChapterPosition
        )
        {
            case ChapterPosition::Start:
            {
                app.readerTopLine =
                    0;

                break;
            }

            case ChapterPosition::End:
            {
                app.readerTopLine =
                    maximumTopLine(
                        app
                    );

                break;
            }

            case ChapterPosition::Restore:
            {
                app.readerTopLine =
                    app.pendingRestoreTopLine;

                clampReaderPosition(
                    app
                );

                break;
            }
        }

        app.pendingChapter =
            ChapterDocument{};

        app.pendingChapterIndex =
            -1;

        app.loadingStage =
            LoadingStage::None;

        app.loadingMessage.clear();
        app.loadingDetail.clear();

        app.screen =
            Screen::Reader;

        clearTextCache(
            app
        );

        std::printf(
            "Now reading chapter %d/%zu: %s\n",
            app.readerChapterIndex + 1,
            app.readerChapterPaths.size(),
            app.readerCurrentChapterPath.c_str()
        );

        std::printf(
            "Wrapped into %zu lines, top=%d\n",
            app.readerLines.size(),
            app.readerTopLine
        );
    }
}

void closeReader(
    App& app)
{
    rememberCurrentProgress(
        app
    );

    app.screen =
        Screen::Library;

    app.readerBookFilename.clear();
    app.readerTitle.clear();

    app.readerChapterPaths.clear();

    app.readerChapterIndex =
        -1;

    app.readerCurrentChapterPath.clear();

    app.readerLines.clear();

    app.readerTopLine =
        0;

    app.pendingBookIndex =
        -1;

    app.pendingChapterIndex =
        -1;

    app.pendingChapter =
        ChapterDocument{};

    clearTextCache(
        app
    );
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

    if (
        TTF_Init() != 0
    )
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
        loadFont(
            17
        );

    app.fontSmall =
        loadFont(
            12
        );

    app.fontReader =
        loadFont(
            14
        );

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
        if (
            !SDL_IsGameController(
                i
            )
        )
        {
            continue;
        }

        app.controller =
            SDL_GameControllerOpen(
                i
            );

        if (
            app.controller
        )
        {
            break;
        }
    }

    loadProgress(
        app
    );

    app.books =
        scanBooks();

    std::printf(
        "Larp Reader: found %zu EPUB(s)\n",
        app.books.size()
    );

    std::printf(
        "Loaded progress for %zu book(s)\n",
        app.savedProgress.size()
    );

    return true;
}

void moveReaderByLines(
    App& app,
    int delta)
{
    if (
        delta > 0 &&
        app.readerTopLine >=
            maximumTopLine(
                app
            )
    )
    {
        const int nextChapter =
            app.readerChapterIndex + 1;

        if (
            nextChapter <
            static_cast<int>(
                app.readerChapterPaths.size()
            )
        )
        {
            rememberCurrentProgress(
                app
            );

            scheduleChapterLoad(
                app,
                nextChapter,
                ChapterPosition::Start,
                0,
                1,
                Screen::Reader
            );
        }

        return;
    }

    if (
        delta < 0 &&
        app.readerTopLine <= 0
    )
    {
        const int previousChapter =
            app.readerChapterIndex - 1;

        if (
            previousChapter >= 0
        )
        {
            rememberCurrentProgress(
                app
            );

            scheduleChapterLoad(
                app,
                previousChapter,
                ChapterPosition::End,
                0,
                -1,
                Screen::Reader
            );
        }

        return;
    }

    app.readerTopLine +=
        delta;

    clampReaderPosition(
        app
    );

    clearTextCache(
        app
    );
}

void moveReaderByPage(
    App& app,
    int direction)
{
    if (
        direction > 0
    )
    {
        const int maxTop =
            maximumTopLine(
                app
            );

        if (
            app.readerTopLine <
            maxTop
        )
        {
            app.readerTopLine =
                std::min(
                    app.readerTopLine
                        + READER_VISIBLE_LINES,
                    maxTop
                );

            clearTextCache(
                app
            );

            return;
        }

        const int nextChapter =
            app.readerChapterIndex + 1;

        if (
            nextChapter <
            static_cast<int>(
                app.readerChapterPaths.size()
            )
        )
        {
            rememberCurrentProgress(
                app
            );

            scheduleChapterLoad(
                app,
                nextChapter,
                ChapterPosition::Start,
                0,
                1,
                Screen::Reader
            );
        }

        return;
    }

    if (
        app.readerTopLine > 0
    )
    {
        app.readerTopLine =
            std::max(
                0,
                app.readerTopLine
                    - READER_VISIBLE_LINES
            );

        clearTextCache(
            app
        );

        return;
    }

    const int previousChapter =
        app.readerChapterIndex - 1;

    if (
        previousChapter >= 0
    )
    {
        rememberCurrentProgress(
            app
        );

        scheduleChapterLoad(
            app,
            previousChapter,
            ChapterPosition::End,
            0,
            -1,
            Screen::Reader
        );
    }
}

void handleLibraryButton(
    App& app,
    Uint8 button)
{
    switch (button)
    {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
        {
            if (
                !app.books.empty()
            )
            {
                --app.selected;

                if (
                    app.selected < 0
                )
                {
                    app.selected =
                        static_cast<int>(
                            app.books.size()
                        )
                        - 1;
                }
            }

            break;
        }

        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        {
            if (
                !app.books.empty()
            )
            {
                ++app.selected;

                if (
                    app.selected >=
                    static_cast<int>(
                        app.books.size()
                    )
                )
                {
                    app.selected =
                        0;
                }
            }

            break;
        }

        case SDL_CONTROLLER_BUTTON_A:
        {
            beginOpenSelectedBook(
                app
            );

            break;
        }

        case SDL_CONTROLLER_BUTTON_START:
        {
            app.running =
                false;

            break;
        }

        default:
            break;
    }
}

void cancelLoading(
    App& app)
{
    app.loadingStage =
        LoadingStage::None;

    app.loadingMessage.clear();
    app.loadingDetail.clear();

    app.pendingChapter =
        ChapterDocument{};

    app.pendingChapterIndex =
        -1;

    if (
        app.loadingReturnScreen ==
            Screen::Reader &&
        !app.readerLines.empty()
    )
    {
        app.screen =
            Screen::Reader;
    }
    else
    {
        app.screen =
            Screen::Library;
    }

    clearTextCache(
        app
    );
}

void handleLoadingButton(
    App& app,
    Uint8 button)
{
    if (
        button ==
        SDL_CONTROLLER_BUTTON_B
    )
    {
        cancelLoading(
            app
        );

        return;
    }

    if (
        button ==
        SDL_CONTROLLER_BUTTON_START
    )
    {
        if (
            !app.readerBookFilename.empty() &&
            app.readerChapterIndex >= 0
        )
        {
            rememberCurrentProgress(
                app
            );
        }

        app.running =
            false;
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
            moveReaderByLines(
                app,
                -1
            );

            break;
        }

        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        {
            moveReaderByLines(
                app,
                1
            );

            break;
        }

        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        {
            moveReaderByPage(
                app,
                -1
            );

            break;
        }

        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
        {
            moveReaderByPage(
                app,
                1
            );

            break;
        }

        case SDL_CONTROLLER_BUTTON_B:
        {
            closeReader(
                app
            );

            break;
        }

        case SDL_CONTROLLER_BUTTON_START:
        {
            rememberCurrentProgress(
                app
            );

            app.running =
                false;

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
            if (
                !app.readerBookFilename.empty() &&
                app.readerChapterIndex >= 0
            )
            {
                rememberCurrentProgress(
                    app
                );
            }

            app.running =
                false;

            continue;
        }

        if (
            event.type ==
            SDL_CONTROLLERDEVICEADDED
        )
        {
            if (
                !app.controller
            )
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

        switch (
            app.screen
        )
        {
            case Screen::Library:
            {
                handleLibraryButton(
                    app,
                    event.cbutton.button
                );

                break;
            }

            case Screen::Loading:
            {
                handleLoadingButton(
                    app,
                    event.cbutton.button
                );

                break;
            }

            case Screen::Reader:
            {
                handleReaderButton(
                    app,
                    event.cbutton.button
                );

                break;
            }
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

    if (
        app.books.empty()
    )
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
        constexpr int firstY =
            57;

        constexpr int rowHeight =
            43;

        constexpr int visibleBooks =
            4;

        int firstBook =
            0;

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
                firstBook
                + row;

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
                + row
                * rowHeight;

            const Book& book =
                app.books[
                    index
                ];

            const bool isSelected =
                index ==
                app.selected;

            if (
                isSelected
            )
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

void renderLoading(
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

    const SDL_Color error =
        {220, 140, 140, 255};

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

    std::string bookTitle =
        app.readerTitle;

    if (
        bookTitle.empty() &&
        app.pendingBookIndex >= 0 &&
        app.pendingBookIndex <
            static_cast<int>(
                app.books.size()
            )
    )
    {
        bookTitle =
            app.books[
                app.pendingBookIndex
            ].title;
    }

    drawText(
        app,
        app.fontSmall,
        bookTitle,
        24,
        78,
        muted
    );

    const bool isError =
        app.loadingStage ==
        LoadingStage::Error;

    drawText(
        app,
        app.fontNormal,
        app.loadingMessage,
        24,
        112,
        isError
            ? error
            : text
    );

    drawText(
        app,
        app.fontSmall,
        app.loadingDetail,
        24,
        145,
        isError
            ? error
            : muted
    );

    drawText(
        app,
        app.fontSmall,
        isError
            ? "O Back"
            : "Please wait...",
        24,
        248,
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

    const std::string chapterIndicator =
        std::to_string(
            app.readerChapterIndex + 1
        )
        + "/"
        + std::to_string(
            app.readerChapterPaths.size()
        );

    drawText(
        app,
        app.fontSmall,
        chapterIndicator,
        430,
        10,
        muted
    );

    int y =
        45;

    for (
        int row = 0;
        row <
            READER_VISIBLE_LINES;
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
            app.readerLines[
                index
            ],
            22,
            y,
            text
        );

        y +=
            READER_LINE_HEIGHT;
    }

    const std::string progress =
        "Page "
        + std::to_string(
            currentPage(
                app
            )
        )
        + "/"
        + std::to_string(
            totalPages(
                app
            )
        );

    drawText(
        app,
        app.fontSmall,
        "L/R Page  UP/DOWN Line  O Back",
        14,
        253,
        muted
    );

    drawText(
        app,
        app.fontSmall,
        progress,
        395,
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
    switch (
        app.screen
    )
    {
        case Screen::Library:
        {
            renderLibrary(
                app
            );

            break;
        }

        case Screen::Loading:
        {
            renderLoading(
                app
            );

            break;
        }

        case Screen::Reader:
        {
            renderReader(
                app
            );

            break;
        }
    }
}

void shutdown(
    App& app)
{
    if (
        !app.readerBookFilename.empty() &&
        app.readerChapterIndex >= 0
    )
    {
        rememberCurrentProgress(
            app
        );
    }

    clearTextCache(
        app
    );

    if (
        app.controller
    )
    {
        SDL_GameControllerClose(
            app.controller
        );
    }

    if (
        app.fontReader
    )
    {
        TTF_CloseFont(
            app.fontReader
        );
    }

    if (
        app.fontSmall
    )
    {
        TTF_CloseFont(
            app.fontSmall
        );
    }

    if (
        app.fontNormal
    )
    {
        TTF_CloseFont(
            app.fontNormal
        );
    }

    if (
        app.renderer
    )
    {
        SDL_DestroyRenderer(
            app.renderer
        );
    }

    if (
        app.window
    )
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

    if (
        !initialize(
            app
        )
    )
    {
        shutdown(
            app
        );

        return 1;
    }

    while (
        app.running
    )
    {
        handleInput(
            app
        );

        render(
            app
        );

        if (
            app.screen ==
            Screen::Loading
        )
        {
            processLoadingStage(
                app
            );
        }

        SDL_Delay(
            16
        );
    }

    shutdown(
        app
    );

    return 0;
}
