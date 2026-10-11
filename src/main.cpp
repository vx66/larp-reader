#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>

#include <tinyxml2.h>
#include <zip.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr int SCREEN_WIDTH = 480;
constexpr int SCREEN_HEIGHT = 272;

constexpr int PORTRAIT_WIDTH = 272;
constexpr int PORTRAIT_HEIGHT = 480;

constexpr int LANDSCAPE_TEXT_LEFT = 22;
constexpr int PORTRAIT_TEXT_LEFT = 16;

constexpr int READER_TOP = 45;
constexpr int READER_LINE_HEIGHT = 18;

constexpr int LEGACY_READER_VISIBLE_LINES = 11;

constexpr int TOC_ROW_HEIGHT = 27;
constexpr int MAX_CAPTION_LINES = 4;

constexpr const char* BOOKS_PATH =
    "ms0:/BOOKS";

constexpr const char* PROGRESS_PATH =
    "ms0:/PSP/GAME/LarpReader/progress.tsv";

constexpr const char* SETTINGS_PATH =
    "ms0:/PSP/GAME/LarpReader/settings.cfg";

constexpr const char* DEBUG_LOG_PATH =
    "ms0:/PSP/GAME/LarpReader/debug.log";

constexpr zip_uint64_t MAX_XML_SIZE =
    4 * 1024 * 1024;

constexpr zip_uint64_t MAX_IMAGE_SIZE =
    16 * 1024 * 1024;

enum class Screen {
    Library,
    Loading,
    Reader,
    Toc
};

enum class LoadingStage {
    None,
    OpenStructure,
    LoadDocument,
    LayoutDocument,
    Error
};

enum class DocumentPosition {
    Start,
    End,
    Restore,
    Semantic
};

enum class BlockKind {
    Text,
    Image
};

enum class PageKind {
    Text,
    Image
};

enum class TextStyle {
    Regular,
    Italic,
    Heading
};

enum class Orientation {
    Landscape,
    Portrait
};

struct CachedText {
    SDL_Texture* texture = nullptr;

    int width = 0;
    int height = 0;
};

struct Book {
    std::string filename;
    std::string path;

    std::string title;
    std::string author;

    std::string opfPath;
    std::string error;

    bool valid = false;
};

struct ManifestItem {
    std::string id;
    std::string href;
    std::string mediaType;
    std::string properties;
};

struct SpineItem {
    std::string path;
    std::string title;
};

struct TocEntry {
    std::string title;

    std::string path;
    std::string fragment;

    int spineIndex = -1;
};

struct ReaderStructure {
    std::vector<SpineItem> spine;
    std::vector<TocEntry> toc;

    std::string error;

    bool valid = false;
};

struct ContentBlock {
    BlockKind kind =
        BlockKind::Text;

    std::string text;
    std::string imagePath;

    bool centered = false;

    TextStyle style =
        TextStyle::Regular;
};

struct AnchorLocation {
    std::size_t blockIndex = 0;
    std::size_t textOffset = 0;

    bool hasTextOffset = false;
};

struct SemanticPosition {
    std::size_t blockIndex = 0;
    std::size_t textOffset = 0;

    bool hasTextOffset = false;
    bool valid = false;
};

struct ChapterDocument {
    std::string path;

    std::vector<ContentBlock> blocks;

    std::unordered_map<
        std::string,
        AnchorLocation
    > anchors;

    std::string error;

    bool valid = false;
    bool skippable = false;
};

struct TextLine {
    std::string text;

    bool centered = false;

    TextStyle style =
        TextStyle::Regular;
};

struct DocumentPage {
    PageKind kind =
        PageKind::Text;

    SemanticPosition sourcePosition;

    std::vector<TextLine> lines;

    std::string imagePath;

    std::vector<TextLine>
        captionLines;
};

struct WrappedTextBlock {
    std::vector<TextLine> lines;

    std::vector<std::size_t>
        lineStarts;
};

struct LayoutResult {
    std::vector<DocumentPage> pages;

    std::unordered_map<
        std::string,
        int
    > anchorPages;
};

struct ReadingProgress {
    int spineIndex = 0;
    int pageIndex = 0;

    SemanticPosition semanticPosition;
};

struct InlineTextBuilder {
    std::string text;

    bool pendingSpace = false;
};

struct App {
    SDL_Window* window = nullptr;

    SDL_Renderer* renderer = nullptr;


    TTF_Font* fontNormal = nullptr;

    TTF_Font* fontSmall = nullptr;
    TTF_Font* fontSmallItalic = nullptr;

    TTF_Font* fontReader = nullptr;
    TTF_Font* fontReaderItalic = nullptr;

    TTF_Font* fontHeading = nullptr;

    SDL_GameController* controller =
        nullptr;

    std::vector<Book> books;

    std::unordered_map<
        std::string,
        CachedText
    > textCache;

    std::unordered_map<
        std::string,
        ReadingProgress
    > savedProgress;

    Screen screen =
        Screen::Library;

    Screen loadingReturnScreen =
        Screen::Library;

    LoadingStage loadingStage =
        LoadingStage::None;

    Orientation orientation =
        Orientation::Landscape;

    int selected = 0;

    int currentBookIndex = -1;

    std::string readerBookFilename;
    std::string readerTitle;

    std::vector<SpineItem>
        readerSpine;

    std::vector<TocEntry>
        readerToc;

    std::vector<DocumentPage>
        readerPages;

    int readerSpineIndex = -1;
    int readerPageIndex = 0;

    SemanticPosition readerSemanticPosition;

    int readerActiveTocIndex = -1;

    int tocSelected = 0;

    SDL_Texture* readerImageTexture =
        nullptr;

    int readerImageWidth = 0;
    int readerImageHeight = 0;

    std::string readerLoadedImagePath;
    std::string readerImageError;

    int pendingSpineIndex = -1;

    int pendingSearchDirection = 1;

    int pendingRestorePage = 0;

    SemanticPosition pendingSemanticPosition;

    int pendingTocIndex = -1;

    std::string pendingFragment;

    DocumentPosition pendingPosition =
        DocumentPosition::Start;

    ChapterDocument pendingDocument;

    std::string loadingMessage;
    std::string loadingDetail;

    int loadingWarmupFrames = 0;

    bool running = true;
};

void debugLog(
    const char* format,
    ...)
{
    FILE* file =
        std::fopen(
            DEBUG_LOG_PATH,
            "a"
        );

    if (!file)
        return;

    va_list args;

    va_start(
        args,
        format
    );

    std::vfprintf(
        file,
        format,
        args
    );

    va_end(
        args
    );

    std::fputc(
        '\n',
        file
    );

    std::fflush(
        file
    );

    std::fclose(
        file
    );
}

void resetDebugLog()
{
    FILE* file =
        std::fopen(
            DEBUG_LOG_PATH,
            "w"
        );

    if (!file)
        return;

    std::fprintf(
        file,
        "Larp Reader 0.0.17 semantic-anchor-fix debug log\n"
    );

    std::fprintf(
        file,
        "============================\n"
    );

    std::fclose(
        file
    );
}

std::string toLower(
    std::string text)
{
    std::transform(
        text.begin(),
        text.end(),
        text.begin(),
        [](unsigned char c)
        {
            return static_cast<char>(
                std::tolower(c)
            );
        }
    );

    return text;
}

bool endsWithEpub(
    const std::string& filename)
{
    if (
        filename.size() < 5
    )
    {
        return false;
    }

    return
        toLower(
            filename.substr(
                filename.size() - 5
            )
        ) == ".epub";
}

int hexValue(
    char c)
{
    if (
        c >= '0' &&
        c <= '9'
    )
    {
        return
            c - '0';
    }

    if (
        c >= 'a' &&
        c <= 'f'
    )
    {
        return
            10
            + c
            - 'a';
    }

    if (
        c >= 'A' &&
        c <= 'F'
    )
    {
        return
            10
            + c
            - 'A';
    }

    return -1;
}

std::string percentDecode(
    const std::string& input)
{
    std::string output;

    output.reserve(
        input.size()
    );

    for (
        std::size_t i = 0;
        i < input.size();
        ++i
    )
    {
        if (
            input[i] == '%' &&
            i + 2 < input.size()
        )
        {
            const int high =
                hexValue(
                    input[i + 1]
                );

            const int low =
                hexValue(
                    input[i + 2]
                );

            if (
                high >= 0 &&
                low >= 0
            )
            {
                output +=
                    static_cast<char>(
                        (high << 4)
                        | low
                    );

                i += 2;

                continue;
            }
        }

        output +=
            input[i];
    }

    return output;
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

    return
        colon
        ? colon + 1
        : name;
}

const char* attributeByLocalName(
    tinyxml2::XMLElement* element,
    const char* wanted)
{
    if (!element)
        return nullptr;

    for (
        const tinyxml2::XMLAttribute* attribute =
            element->FirstAttribute();

        attribute;

        attribute =
            attribute->Next()
    )
    {
        if (
            std::strcmp(
                localName(
                    attribute->Name()
                ),
                wanted
            ) == 0
        )
        {
            return
                attribute->Value();
        }
    }

    return nullptr;
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

        if (
            element &&
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

tinyxml2::XMLElement*
findDirectChildByLocalName(
    tinyxml2::XMLElement* parent,
    const char* wanted)
{
    if (!parent)
        return nullptr;

    for (
        tinyxml2::XMLElement* child =
            parent->FirstChildElement();

        child;

        child =
            child->NextSiblingElement()
    )
    {
        if (
            std::strcmp(
                localName(
                    child->Name()
                ),
                wanted
            ) == 0
        )
        {
            return child;
        }
    }

    return nullptr;
}

std::string elementText(
    tinyxml2::XMLElement* element)
{
    if (
        !element ||
        !element->GetText()
    )
    {
        return {};
    }

    return
        element->GetText();
}

void replaceAll(
    std::string& value,
    const std::string& from,
    const std::string& to)
{
    if (
        from.empty()
    )
    {
        return;
    }

    std::size_t pos = 0;

    while (
        (
            pos =
                value.find(
                    from,
                    pos
                )
        ) !=
        std::string::npos
    )
    {
        value.replace(
            pos,
            from.size(),
            to
        );

        pos +=
            to.size();
    }
}

std::string sanitizeXhtmlEntities(
    std::string xml)
{
    replaceAll(
        xml,
        "&nbsp;",
        "&#160;"
    );

    replaceAll(
        xml,
        "&ndash;",
        "&#8211;"
    );

    replaceAll(
        xml,
        "&mdash;",
        "&#8212;"
    );

    replaceAll(
        xml,
        "&hellip;",
        "&#8230;"
    );

    replaceAll(
        xml,
        "&lsquo;",
        "&#8216;"
    );

    replaceAll(
        xml,
        "&rsquo;",
        "&#8217;"
    );

    replaceAll(
        xml,
        "&ldquo;",
        "&#8220;"
    );

    replaceAll(
        xml,
        "&rdquo;",
        "&#8221;"
    );

    replaceAll(
        xml,
        "&laquo;",
        "&#171;"
    );

    replaceAll(
        xml,
        "&raquo;",
        "&#187;"
    );

    replaceAll(
        xml,
        "&copy;",
        "&#169;"
    );

    replaceAll(
        xml,
        "&reg;",
        "&#174;"
    );

    replaceAll(
        xml,
        "&trade;",
        "&#8482;"
    );

    return xml;
}

std::string normalizeText(
    const std::string& input)
{
    std::string output;

    bool pendingSpace =
        false;

    for (
        std::size_t i = 0;
        i < input.size();
        ++i
    )
    {
        const unsigned char c =
            static_cast<unsigned char>(
                input[i]
            );

        if (
            c == 0xC2 &&
            i + 1 < input.size() &&
            static_cast<unsigned char>(
                input[i + 1]
            ) == 0xA0
        )
        {
            pendingSpace =
                !output.empty();

            ++i;

            continue;
        }

        if (
            std::isspace(c)
        )
        {
            pendingSpace =
                !output.empty();

            continue;
        }

        if (
            pendingSpace &&
            !output.empty()
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
        output.back() == ' '
    )
    {
        output.pop_back();
    }

    return output;
}

void appendNodeText(
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
        if (
            text->Value()
        )
        {
            output +=
                text->Value();
        }

        return;
    }

    tinyxml2::XMLElement* element =
        node->ToElement();

    if (
        element
    )
    {
        const std::string name =
            toLower(
                localName(
                    element->Name()
                )
            );

        if (
            name == "style" ||
            name == "script" ||
            name == "img" ||
            name == "svg"
        )
        {
            return;
        }

        if (
            name == "br"
        )
        {
            output += ' ';
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
        appendNodeText(
            child,
            output
        );
    }
}

std::string nodeText(
    tinyxml2::XMLNode* node)
{
    std::string raw;

    appendNodeText(
        node,
        raw
    );

    return
        normalizeText(
            raw
        );
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
                    + static_cast<std::size_t>(
                        totalRead
                    ),
                statInfo.size
                    - totalRead
            );

        if (
            readNow < 0
        )
        {
            zip_fclose(
                file
            );

            error =
                "Error reading ZIP entry: "
                + entryName;

            return false;
        }

        if (
            readNow == 0
        )
        {
            break;
        }

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

bool readZipEntryBytes(
    zip_t* archive,
    const std::string& entryName,
    std::vector<std::uint8_t>& output,
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
            "Missing image: "
            + entryName;

        return false;
    }

    if (
        statInfo.size >
        MAX_IMAGE_SIZE
    )
    {
        error =
            "Image too large";

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
            "Could not open image";

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
                    + static_cast<std::size_t>(
                        totalRead
                    ),
                statInfo.size
                    - totalRead
            );

        if (
            readNow < 0
        )
        {
            zip_fclose(
                file
            );

            error =
                "Could not read image";

            return false;
        }

        if (
            readNow == 0
        )
        {
            break;
        }

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

    debugLog(
        "Image bytes: %s = %llu",
        entryName.c_str(),
        static_cast<unsigned long long>(
            totalRead
        )
    );

    return true;
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

    return
        path.substr(
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
            slash ==
                std::string::npos
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
        if (
            i > 0
        )
        {
            result += '/';
        }

        result +=
            parts[i];
    }

    return result;
}

struct ArchiveReference {
    std::string path;
    std::string fragment;
};

ArchiveReference resolveArchiveReference(
    const std::string& baseDocumentPath,
    const std::string& href)
{
    ArchiveReference result;

    std::string cleanHref =
        href;

    const std::size_t fragmentPos =
        cleanHref.find(
            '#'
        );

    if (
        fragmentPos !=
        std::string::npos
    )
    {
        result.fragment =
            percentDecode(
                cleanHref.substr(
                    fragmentPos + 1
                )
            );

        cleanHref.erase(
            fragmentPos
        );
    }

    const std::size_t queryPos =
        cleanHref.find(
            '?'
        );

    if (
        queryPos !=
        std::string::npos
    )
    {
        cleanHref.erase(
            queryPos
        );
    }

    cleanHref =
        percentDecode(
            cleanHref
        );

    if (
        cleanHref.empty()
    )
    {
        result.path =
            normalizeArchivePath(
                baseDocumentPath
            );
    }
    else
    {
        result.path =
            normalizeArchivePath(
                archiveDirectory(
                    baseDocumentPath
                )
                + cleanHref
            );
    }

    return result;
}

std::string resolveArchivePath(
    const std::string& baseDocumentPath,
    const std::string& href)
{
    return
        resolveArchiveReference(
            baseDocumentPath,
            href
        ).path;
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
        ) !=
        tinyxml2::XML_SUCCESS
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
        ) !=
        tinyxml2::XML_SUCCESS
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

    book.title =
        elementText(
            findElementByLocalName(
                metadata,
                "title"
            )
        );

    book.author =
        elementText(
            findElementByLocalName(
                metadata,
                "creator"
            )
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
        const std::string filename =
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

void appendNavEntry(
    std::vector<TocEntry>& entries,
    const std::string& title,
    const ArchiveReference& reference)
{
    if (
        title.empty() ||
        reference.path.empty()
    )
    {
        return;
    }

    for (
        const TocEntry& existing :
        entries
    )
    {
        if (
            existing.path ==
                reference.path &&
            existing.fragment ==
                reference.fragment &&
            existing.title ==
                title
        )
        {
            return;
        }
    }

    TocEntry entry;

    entry.title =
        title;

    entry.path =
        reference.path;

    entry.fragment =
        reference.fragment;

    entries.push_back(
        std::move(
            entry
        )
    );
}

tinyxml2::XMLElement*
findNavToc(
    tinyxml2::XMLNode* node)
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

        if (
            element &&
            std::strcmp(
                localName(
                    element->Name()
                ),
                "nav"
            ) == 0
        )
        {
            const char* type =
                attributeByLocalName(
                    element,
                    "type"
                );

            if (
                type &&
                propertyContainsToken(
                    type,
                    "toc"
                )
            )
            {
                return element;
            }
        }

        tinyxml2::XMLElement* nested =
            findNavToc(
                child
            );

        if (nested)
            return nested;
    }

    return nullptr;
}

void collectNavLinks(
    tinyxml2::XMLNode* node,
    const std::string& navPath,
    std::vector<TocEntry>& entries)
{
    if (!node)
        return;

    tinyxml2::XMLElement* element =
        node->ToElement();

    if (
        element &&
        std::strcmp(
            localName(
                element->Name()
            ),
            "a"
        ) == 0
    )
    {
        const char* href =
            attributeByLocalName(
                element,
                "href"
            );

        if (
            href &&
            href[0] != '\0'
        )
        {
            appendNavEntry(
                entries,
                nodeText(
                    element
                ),
                resolveArchiveReference(
                    navPath,
                    href
                )
            );
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
        collectNavLinks(
            child,
            navPath,
            entries
        );
    }
}

void collectNcxNavPoints(
    tinyxml2::XMLNode* node,
    const std::string& ncxPath,
    std::vector<TocEntry>& entries)
{
    if (!node)
        return;

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

        if (!element)
            continue;

        if (
            std::strcmp(
                localName(
                    element->Name()
                ),
                "navPoint"
            ) == 0
        )
        {
            tinyxml2::XMLElement* navLabel =
                findDirectChildByLocalName(
                    element,
                    "navLabel"
                );

            tinyxml2::XMLElement* textElement =
                findDirectChildByLocalName(
                    navLabel,
                    "text"
                );

            tinyxml2::XMLElement* content =
                findDirectChildByLocalName(
                    element,
                    "content"
                );

            const char* src =
                content
                ? content->Attribute(
                    "src"
                )
                : nullptr;

            if (
                src &&
                src[0] != '\0'
            )
            {
                const std::string title =
                    textElement
                    ? nodeText(
                        textElement
                    )
                    : nodeText(
                        navLabel
                    );

                appendNavEntry(
                    entries,
                    title,
                    resolveArchiveReference(
                        ncxPath,
                        src
                    )
                );
            }
        }

        collectNcxNavPoints(
            element,
            ncxPath,
            entries
        );
    }
}

std::vector<TocEntry>
loadNavigationEntries(
    zip_t* archive,
    const std::string& opfPath,
    const std::unordered_map<
        std::string,
        ManifestItem
    >& manifestItems,
    tinyxml2::XMLElement* spine)
{
    std::vector<TocEntry>
        navEntries;

    for (
        const auto& pair :
        manifestItems
    )
    {
        const ManifestItem& item =
            pair.second;

        if (
            item.mediaType !=
                "application/xhtml+xml" ||
            !propertyContainsToken(
                item.properties,
                "nav"
            )
        )
        {
            continue;
        }

        const std::string navPath =
            resolveArchivePath(
                opfPath,
                item.href
            );

        std::string xml;
        std::string error;

        if (
            !readZipEntry(
                archive,
                navPath,
                xml,
                error
            )
        )
        {
            continue;
        }

        xml =
            sanitizeXhtmlEntities(
                std::move(
                    xml
                )
            );

        tinyxml2::XMLDocument document;

        if (
            document.Parse(
                xml.data(),
                xml.size()
            ) !=
            tinyxml2::XML_SUCCESS
        )
        {
            continue;
        }

        tinyxml2::XMLElement* nav =
            findNavToc(
                &document
            );

        if (!nav)
        {
            nav =
                findElementByLocalName(
                    &document,
                    "nav"
                );
        }

        if (
            nav
        )
        {
            collectNavLinks(
                nav,
                navPath,
                navEntries
            );
        }
    }

    if (
        !navEntries.empty()
    )
    {
        debugLog(
            "Using EPUB 3 NAV: %zu entries",
            navEntries.size()
        );

        return navEntries;
    }

    const ManifestItem* ncxItem =
        nullptr;

    const char* tocId =
        spine
        ? spine->Attribute(
            "toc"
        )
        : nullptr;

    if (
        tocId
    )
    {
        auto found =
            manifestItems.find(
                tocId
            );

        if (
            found !=
            manifestItems.end()
        )
        {
            ncxItem =
                &found->second;
        }
    }

    if (!ncxItem)
    {
        for (
            const auto& pair :
            manifestItems
        )
        {
            if (
                pair.second.mediaType ==
                "application/x-dtbncx+xml"
            )
            {
                ncxItem =
                    &pair.second;

                break;
            }
        }
    }

    if (!ncxItem)
        return {};

    const std::string ncxPath =
        resolveArchivePath(
            opfPath,
            ncxItem->href
        );

    std::string xml;
    std::string error;

    if (
        !readZipEntry(
            archive,
            ncxPath,
            xml,
            error
        )
    )
    {
        return {};
    }

    tinyxml2::XMLDocument document;

    if (
        document.Parse(
            xml.data(),
            xml.size()
        ) !=
        tinyxml2::XML_SUCCESS
    )
    {
        return {};
    }

    tinyxml2::XMLElement* navMap =
        findElementByLocalName(
            &document,
            "navMap"
        );

    std::vector<TocEntry>
        ncxEntries;

    if (
        navMap
    )
    {
        collectNcxNavPoints(
            navMap,
            ncxPath,
            ncxEntries
        );
    }

    debugLog(
        "Using EPUB 2 NCX: %zu entries",
        ncxEntries.size()
    );

    return ncxEntries;
}

std::string fallbackSpineTitle(
    const std::string& path,
    int index)
{
    std::string filename =
        path;

    const std::size_t slash =
        filename.find_last_of(
            '/'
        );

    if (
        slash !=
        std::string::npos
    )
    {
        filename.erase(
            0,
            slash + 1
        );
    }

    const std::string lower =
        toLower(
            filename
        );

    if (
        lower.find(
            "cover"
        ) !=
            std::string::npos ||
        lower.find(
            "cubierta"
        ) !=
            std::string::npos
    )
    {
        return "Cover";
    }

    if (
        lower.find(
            "title"
        ) !=
        std::string::npos
    )
    {
        return "Title page";
    }

    if (
        lower.find(
            "prologo"
        ) !=
        std::string::npos
    )
    {
        return "Prólogo";
    }

    if (
        lower.find(
            "epilogo"
        ) !=
        std::string::npos
    )
    {
        return "Epílogo";
    }

    if (
        lower.find(
            "autor"
        ) !=
        std::string::npos
    )
    {
        return "Autor";
    }

    if (
        lower.find(
            "editor"
        ) !=
        std::string::npos
    )
    {
        return "Editor";
    }

    if (
        lower.find(
            "info"
        ) !=
        std::string::npos
    )
    {
        return "Información";
    }

    if (
        lower.find(
            "sinopsis"
        ) !=
        std::string::npos
    )
    {
        return "Sinopsis";
    }

    if (
        lower.find(
            "about"
        ) !=
        std::string::npos
    )
    {
        return "About";
    }

    if (
        lower.find(
            "copyright"
        ) !=
        std::string::npos
    )
    {
        return "Copyright";
    }

    return
        "Section "
        + std::to_string(
            index + 1
        );
}

int findSpineIndexByPath(
    const std::vector<SpineItem>& spine,
    const std::string& path)
{
    for (
        std::size_t i = 0;
        i < spine.size();
        ++i
    )
    {
        if (
            spine[i].path ==
            path
        )
        {
            return
                static_cast<int>(
                    i
                );
        }
    }

    return -1;
}

bool tocHasSpineIndex(
    const std::vector<TocEntry>& toc,
    int spineIndex)
{
    for (
        const TocEntry& entry :
        toc
    )
    {
        if (
            entry.spineIndex ==
            spineIndex
        )
        {
            return true;
        }
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

    tinyxml2::XMLDocument document;

    if (
        document.Parse(
            opfXml.data(),
            opfXml.size()
        ) !=
        tinyxml2::XML_SUCCESS
    )
    {
        result.error =
            "Invalid OPF";

        zip_close(
            archive
        );

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

        zip_close(
            archive
        );

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

        if (
            const char* media =
                item->Attribute(
                    "media-type"
                )
        )
        {
            manifestItem.mediaType =
                media;
        }

        if (
            const char* properties =
                item->Attribute(
                    "properties"
                )
        )
        {
            manifestItem.properties =
                properties;
        }

        manifestItems[
            manifestItem.id
        ] =
            std::move(
                manifestItem
            );
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

        if (
            propertyContainsToken(
                item.properties,
                "nav"
            )
        )
        {
            continue;
        }

        SpineItem spineItem;

        spineItem.path =
            resolveArchivePath(
                book.opfPath,
                item.href
            );

        if (
            spineItem.path.empty()
        )
        {
            continue;
        }

        spineItem.title =
            fallbackSpineTitle(
                spineItem.path,
                static_cast<int>(
                    result.spine.size()
                )
            );

        result.spine.push_back(
            std::move(
                spineItem
            )
        );
    }

    if (
        result.spine.empty()
    )
    {
        result.error =
            "No readable spine items";

        zip_close(
            archive
        );

        return result;
    }

    std::vector<TocEntry> rawToc =
        loadNavigationEntries(
            archive,
            book.opfPath,
            manifestItems,
            spine
        );

    zip_close(
        archive
    );

    for (
        TocEntry entry :
        rawToc
    )
    {
        entry.spineIndex =
            findSpineIndexByPath(
                result.spine,
                entry.path
            );

        if (
            entry.spineIndex < 0
        )
        {
            continue;
        }

        result.toc.push_back(
            std::move(
                entry
            )
        );
    }

    if (
        !result.spine.empty() &&
        !tocHasSpineIndex(
            result.toc,
            0
        )
    )
    {
        const std::string firstLower =
            toLower(
                result.spine[0].path
            );

        if (
            firstLower.find(
                "cover"
            ) !=
                std::string::npos ||
            firstLower.find(
                "cubierta"
            ) !=
                std::string::npos
        )
        {
            TocEntry cover;

            cover.title =
                "Cover";

            cover.path =
                result.spine[0].path;

            cover.spineIndex =
                0;

            result.toc.insert(
                result.toc.begin(),
                std::move(
                    cover
                )
            );
        }
    }

    if (
        result.toc.empty()
    )
    {
        for (
            std::size_t i = 0;
            i < result.spine.size();
            ++i
        )
        {
            TocEntry entry;

            entry.title =
                result.spine[i].title;

            entry.path =
                result.spine[i].path;

            entry.spineIndex =
                static_cast<int>(
                    i
                );

            result.toc.push_back(
                std::move(
                    entry
                )
            );
        }
    }

    for (
        const TocEntry& entry :
        result.toc
    )
    {
        if (
            entry.fragment.empty()
        )
        {
            result.spine[
                entry.spineIndex
            ].title =
                entry.title;
        }
    }

    for (
        std::size_t i = 0;
        i < result.spine.size();
        ++i
    )
    {
        debugLog(
            "SPINE %zu: %s -> %s",
            i + 1,
            result.spine[i].path.c_str(),
            result.spine[i].title.c_str()
        );
    }

    for (
        std::size_t i = 0;
        i < result.toc.size();
        ++i
    )
    {
        debugLog(
            "TOC %zu: spine=%d path=%s fragment=%s title=%s",
            i + 1,
            result.toc[i].spineIndex + 1,
            result.toc[i].path.c_str(),
            result.toc[i].fragment.empty()
                ? "(none)"
                : result.toc[i].fragment.c_str(),
            result.toc[i].title.c_str()
        );
    }

    result.valid =
        true;

    return result;
}

bool isIgnoredElementName(
    const std::string& name)
{
    return
        name == "head" ||
        name == "style" ||
        name == "script" ||
        name == "noscript";
}

bool isTextBlockName(
    const std::string& name)
{
    return
        name == "p" ||
        name == "div" ||
        name == "section" ||
        name == "article" ||
        name == "blockquote" ||
        name == "li" ||
        name == "dt" ||
        name == "dd" ||
        name == "h1" ||
        name == "h2" ||
        name == "h3" ||
        name == "h4" ||
        name == "h5" ||
        name == "h6" ||
        name == "pre";
}

bool isHeadingName(
    const std::string& name)
{
    return
        name == "h1" ||
        name == "h2" ||
        name == "h3" ||
        name == "h4" ||
        name == "h5" ||
        name == "h6";
}

bool classContains(
    const char* classValue,
    const char* token)
{
    if (
        !classValue ||
        !token
    )
    {
        return false;
    }

    const std::string classes =
        toLower(
            classValue
        );

    return
        classes.find(
            toLower(
                token
            )
        ) !=
        std::string::npos;
}

bool styleContainsCentered(
    const char* styleValue)
{
    if (!styleValue)
        return false;

    const std::string style =
        toLower(
            styleValue
        );

    return
        style.find(
            "text-align"
        ) !=
            std::string::npos &&
        style.find(
            "center"
        ) !=
            std::string::npos;
}

bool elementIsCentered(
    tinyxml2::XMLElement* element)
{
    if (!element)
        return false;

    const std::string name =
        toLower(
            localName(
                element->Name()
            )
        );

    if (
        isHeadingName(
            name
        )
    )
    {
        return true;
    }

    const char* classes =
        element->Attribute(
            "class"
        );

    if (
        classContains(
            classes,
            "centrado"
        ) ||
        classContains(
            classes,
            "capitulo"
        ) ||
        classContains(
            classes,
            "title"
        ) ||
        classContains(
            classes,
            "subtitle"
        ) ||
        classContains(
            classes,
            "separador"
        ) ||
        classContains(
            classes,
            "fin"
        )
    )
    {
        return true;
    }

    return
        styleContainsCentered(
            element->Attribute(
                "style"
            )
        );
}

bool hasNonWhitespaceDirectText(
    tinyxml2::XMLElement* element)
{
    if (!element)
        return false;

    for (
        tinyxml2::XMLNode* child =
            element->FirstChild();

        child;

        child =
            child->NextSibling()
    )
    {
        tinyxml2::XMLText* text =
            child->ToText();

        if (
            !text ||
            !text->Value()
        )
        {
            continue;
        }

        const unsigned char* value =
            reinterpret_cast<
                const unsigned char*
            >(
                text->Value()
            );

        for (
            const unsigned char* p =
                value;

            *p;

            ++p
        )
        {
            if (
                !std::isspace(
                    *p
                )
            )
            {
                return true;
            }
        }
    }

    return false;
}

bool blockIsEntirelyItalic(
    tinyxml2::XMLElement* element)
{
    if (!element)
        return false;

    const char* styleValue =
        element->Attribute(
            "style"
        );

    if (
        styleValue
    )
    {
        const std::string style =
            toLower(
                styleValue
            );

        if (
            style.find(
                "font-style"
            ) !=
                std::string::npos &&
            style.find(
                "italic"
            ) !=
                std::string::npos
        )
        {
            return true;
        }
    }

    const char* classValue =
        element->Attribute(
            "class"
        );

    if (
        classValue
    )
    {
        const std::string classes =
            toLower(
                classValue
            );

        if (
            classes.find(
                "italic"
            ) !=
                std::string::npos ||
            classes.find(
                "cursiva"
            ) !=
                std::string::npos
        )
        {
            return true;
        }
    }

    if (
        hasNonWhitespaceDirectText(
            element
        )
    )
    {
        return false;
    }

    bool foundStyledChild =
        false;

    for (
        tinyxml2::XMLElement* child =
            element->FirstChildElement();

        child;

        child =
            child->NextSiblingElement()
    )
    {
        const std::string childName =
            toLower(
                localName(
                    child->Name()
                )
            );

        if (
            childName == "i" ||
            childName == "em"
        )
        {
            foundStyledChild =
                true;

            continue;
        }

        if (
            childName == "br"
        )
        {
            continue;
        }

        return false;
    }

    return
        foundStyledChild;
}

TextStyle textStyleForElement(
    tinyxml2::XMLElement* element,
    const std::string& name)
{
    if (
        isHeadingName(
            name
        )
    )
    {
        return
            TextStyle::Heading;
    }

    if (
        blockIsEntirelyItalic(
            element
        )
    )
    {
        return
            TextStyle::Italic;
    }

    return
        TextStyle::Regular;
}

bool hasDirectStructuralChild(
    tinyxml2::XMLElement* element)
{
    if (!element)
        return false;

    for (
        tinyxml2::XMLElement* child =
            element->FirstChildElement();

        child;

        child =
            child->NextSiblingElement()
    )
    {
        const std::string name =
            toLower(
                localName(
                    child->Name()
                )
            );

        if (
            name == "img" ||
            name == "svg" ||
            isTextBlockName(
                name
            )
        )
        {
            return true;
        }
    }

    return false;
}

void materializePendingSpace(
    InlineTextBuilder& builder)
{
    if (
        builder.pendingSpace &&
        !builder.text.empty()
    )
    {
        builder.text += ' ';
    }

    builder.pendingSpace =
        false;
}

void appendNormalizedTextChunk(
    InlineTextBuilder& builder,
    const char* value)
{
    if (!value)
        return;

    const std::string input =
        value;

    for (
        std::size_t i = 0;
        i < input.size();
        ++i
    )
    {
        const unsigned char c =
            static_cast<unsigned char>(
                input[i]
            );

        if (
            c == 0xC2 &&
            i + 1 < input.size() &&
            static_cast<unsigned char>(
                input[i + 1]
            ) == 0xA0
        )
        {
            builder.pendingSpace =
                !builder.text.empty();

            ++i;

            continue;
        }

        if (
            std::isspace(c)
        )
        {
            builder.pendingSpace =
                !builder.text.empty();

            continue;
        }

        if (
            builder.pendingSpace &&
            !builder.text.empty()
        )
        {
            builder.text += ' ';
        }

        builder.pendingSpace =
            false;

        builder.text +=
            static_cast<char>(
                c
            );
    }
}

void recordAnchorName(
    const char* value,
    const AnchorLocation& location,
    std::unordered_map<
        std::string,
        AnchorLocation
    >& anchors)
{
    if (
        !value ||
        value[0] == '\0'
    )
    {
        return;
    }

    anchors[
        value
    ] =
        location;
}

void recordElementAnchors(
    tinyxml2::XMLElement* element,
    const AnchorLocation& location,
    std::unordered_map<
        std::string,
        AnchorLocation
    >& anchors)
{
    if (!element)
        return;

    recordAnchorName(
        attributeByLocalName(
            element,
            "id"
        ),
        location,
        anchors
    );

    recordAnchorName(
        attributeByLocalName(
            element,
            "name"
        ),
        location,
        anchors
    );
}

void collectInlineTextAndAnchors(
    tinyxml2::XMLNode* node,
    std::size_t blockIndex,
    InlineTextBuilder& builder,
    std::unordered_map<
        std::string,
        AnchorLocation
    >& anchors)
{
    if (!node)
        return;

    if (
        tinyxml2::XMLText* text =
            node->ToText()
    )
    {
        appendNormalizedTextChunk(
            builder,
            text->Value()
        );

        return;
    }

    tinyxml2::XMLElement* element =
        node->ToElement();

    if (
        element
    )
    {
        const std::string name =
            toLower(
                localName(
                    element->Name()
                )
            );

        if (
            isIgnoredElementName(
                name
            ) ||
            name == "img" ||
            name == "svg"
        )
        {
            return;
        }

        materializePendingSpace(
            builder
        );

        AnchorLocation location;

        location.blockIndex =
            blockIndex;

        location.textOffset =
            builder.text.size();

        location.hasTextOffset =
            true;

        recordElementAnchors(
            element,
            location,
            anchors
        );

        if (
            name == "br"
        )
        {
            builder.pendingSpace =
                !builder.text.empty();

            return;
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
        collectInlineTextAndAnchors(
            child,
            blockIndex,
            builder,
            anchors
        );
    }
}

std::string firstSvgImageReference(
    tinyxml2::XMLNode* node)
{
    if (!node)
        return {};

    tinyxml2::XMLElement* element =
        node->ToElement();

    if (
        element
    )
    {
        const std::string name =
            toLower(
                localName(
                    element->Name()
                )
            );

        if (
            name == "image"
        )
        {
            const char* href =
                attributeByLocalName(
                    element,
                    "href"
                );

            if (
                href &&
                href[0] != '\0'
            )
            {
                return href;
            }
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
        const std::string result =
            firstSvgImageReference(
                child
            );

        if (
            !result.empty()
        )
        {
            return result;
        }
    }

    return {};
}

void collectContentBlocks(
    tinyxml2::XMLNode* node,
    const std::string& xhtmlPath,
    bool inheritedCentered,
    std::vector<ContentBlock>& blocks,
    std::unordered_map<
        std::string,
        AnchorLocation
    >& anchors)
{
    if (!node)
        return;

    tinyxml2::XMLElement* element =
        node->ToElement();

    if (!element)
    {
        for (
            tinyxml2::XMLNode* child =
                node->FirstChild();

            child;

            child =
                child->NextSibling()
        )
        {
            collectContentBlocks(
                child,
                xhtmlPath,
                inheritedCentered,
                blocks,
                anchors
            );
        }

        return;
    }

    const std::string name =
        toLower(
            localName(
                element->Name()
            )
        );

    if (
        isIgnoredElementName(
            name
        )
    )
    {
        return;
    }

    AnchorLocation outerLocation;

    outerLocation.blockIndex =
        blocks.size();

    outerLocation.textOffset =
        0;

    outerLocation.hasTextOffset =
        false;

    recordElementAnchors(
        element,
        outerLocation,
        anchors
    );

    const bool centered =
        inheritedCentered ||
        elementIsCentered(
            element
        );

    if (
        name == "img"
    )
    {
        const char* src =
            attributeByLocalName(
                element,
                "src"
            );

        if (
            src &&
            src[0] != '\0'
        )
        {
            ContentBlock block;

            block.kind =
                BlockKind::Image;

            block.imagePath =
                resolveArchivePath(
                    xhtmlPath,
                    src
                );

            block.centered =
                true;

            blocks.push_back(
                std::move(
                    block
                )
            );
        }

        return;
    }

    if (
        name == "svg"
    )
    {
        const std::string href =
            firstSvgImageReference(
                element
            );

        if (
            !href.empty()
        )
        {
            ContentBlock block;

            block.kind =
                BlockKind::Image;

            block.imagePath =
                resolveArchivePath(
                    xhtmlPath,
                    href
                );

            block.centered =
                true;

            blocks.push_back(
                std::move(
                    block
                )
            );
        }

        return;
    }

    if (
        isTextBlockName(
            name
        ) &&
        !hasDirectStructuralChild(
            element
        )
    )
    {
        const std::size_t blockIndex =
            blocks.size();

        InlineTextBuilder builder;

        collectInlineTextAndAnchors(
            element,
            blockIndex,
            builder,
            anchors
        );

        while (
            !builder.text.empty() &&
            builder.text.back() == ' '
        )
        {
            builder.text.pop_back();
        }

        if (
            !builder.text.empty()
        )
        {
            ContentBlock block;

            block.kind =
                BlockKind::Text;

            block.text =
                std::move(
                    builder.text
                );

            block.centered =
                centered;

            block.style =
                textStyleForElement(
                    element,
                    name
                );

            blocks.push_back(
                std::move(
                    block
                )
            );
        }

        return;
    }

    for (
        tinyxml2::XMLNode* child =
            element->FirstChild();

        child;

        child =
            child->NextSibling()
    )
    {
        collectContentBlocks(
            child,
            xhtmlPath,
            centered,
            blocks,
            anchors
        );
    }
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

    std::string xml;

    if (
        !readZipEntry(
            archive,
            chapterPath,
            xml,
            result.error
        )
    )
    {
        zip_close(
            archive
        );

        return result;
    }

    zip_close(
        archive
    );

    xml =
        sanitizeXhtmlEntities(
            std::move(
                xml
            )
        );

    tinyxml2::XMLDocument document;

    const tinyxml2::XMLError parseResult =
        document.Parse(
            xml.data(),
            xml.size()
        );

    if (
        parseResult !=
        tinyxml2::XML_SUCCESS
    )
    {
        result.error =
            "Invalid XHTML";

        debugLog(
            "XHTML parse failed: %s error=%d",
            chapterPath.c_str(),
            static_cast<int>(
                parseResult
            )
        );

        return result;
    }

    tinyxml2::XMLElement* body =
        findElementByLocalName(
            &document,
            "body"
        );

    if (!body)
    {
        result.error =
            "XHTML has no body";

        result.skippable =
            true;

        return result;
    }

    collectContentBlocks(
        body,
        chapterPath,
        elementIsCentered(
            body
        ),
        result.blocks,
        result.anchors
    );

    if (
        result.blocks.empty()
    )
    {
        result.error =
            "Spine item contains no supported content";

        result.skippable =
            true;

        return result;
    }

    int textBlocks = 0;
    int imageBlocks = 0;

    for (
        const ContentBlock& block :
        result.blocks
    )
    {
        if (
            block.kind ==
            BlockKind::Image
        )
        {
            ++imageBlocks;
        }
        else
        {
            ++textBlocks;
        }
    }

    debugLog(
        "DOCUMENT: %s blocks=%zu text=%d images=%d anchors=%zu",
        chapterPath.c_str(),
        result.blocks.size(),
        textBlocks,
        imageBlocks,
        result.anchors.size()
    );

    for (
        const auto& pair :
        result.anchors
    )
    {
        debugLog(
            "ANCHOR: %s -> block=%zu offset=%zu exact=%s",
            pair.first.c_str(),
            pair.second.blockIndex,
            pair.second.textOffset,
            pair.second.hasTextOffset
                ? "yes"
                : "no"
        );
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

        if (
            font
        )
        {
            return font;
        }
    }

    return nullptr;
}

int canvasWidth(
    const App& app)
{
    return
        app.orientation ==
            Orientation::Portrait
        ? PORTRAIT_WIDTH
        : SCREEN_WIDTH;
}

int canvasHeight(
    const App& app)
{
    return
        app.orientation ==
            Orientation::Portrait
        ? PORTRAIT_HEIGHT
        : SCREEN_HEIGHT;
}

int readerTextLeft(
    const App& app)
{
    return
        app.orientation ==
            Orientation::Portrait
        ? PORTRAIT_TEXT_LEFT
        : LANDSCAPE_TEXT_LEFT;
}

int readerTextWidth(
    const App& app)
{
    return
        canvasWidth(
            app
        )
        - readerTextLeft(
            app
        )
        * 2;
}

int readerContentBottom(
    const App& app)
{
    if (
        app.orientation ==
        Orientation::Portrait
    )
    {
        return
            canvasHeight(
                app
            )
            - 46;
    }

    return 242;
}

int readerVisibleLines(
    const App& app)
{
    return
        std::max(
            1,
            (
                readerContentBottom(
                    app
                )
                - READER_TOP
            )
            / READER_LINE_HEIGHT
            + 1
        );
}

int tocVisibleItems(
    const App& app)
{
    if (
        app.orientation ==
        Orientation::Portrait
    )
    {
        const int top =
            60;

        const int bottom =
            canvasHeight(
                app
            )
            - 28;

        return
            std::max(
                1,
                (
                    bottom
                    - top
                )
                / TOC_ROW_HEIGHT
            );
    }

    return 7;
}

int textPixelWidth(
    TTF_Font* font,
    const std::string& text)
{
    if (
        !font ||
        text.empty()
    )
    {
        return 0;
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
        return 0;
    }

    return width;
}

std::string fitTextToWidth(
    TTF_Font* font,
    const std::string& text,
    int maxWidth)
{
    if (
        textPixelWidth(
            font,
            text
        ) <=
        maxWidth
    )
    {
        return text;
    }

    const std::string ellipsis =
        "...";

    std::string result =
        text;

    while (
        !result.empty()
    )
    {
        std::size_t start =
            result.size() - 1;

        while (
            start > 0 &&
            (
                static_cast<unsigned char>(
                    result[start]
                )
                & 0xC0
            ) == 0x80
        )
        {
            --start;
        }

        result.erase(
            start
        );

        if (
            textPixelWidth(
                font,
                result + ellipsis
            ) <=
            maxWidth
        )
        {
            return
                result
                + ellipsis;
        }
    }

    return ellipsis;
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
        return
            found->second;
    }

    const int width =
        textPixelWidth(
            font,
            text
        );

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

void wrapParagraphFast(
    TTF_Font* font,
    const std::string& paragraph,
    int maxWidth,
    int spaceWidth,
    std::unordered_map<
        std::string,
        int
    >& widthCache,
    std::vector<std::string>& lines,
    std::vector<std::size_t>& lineStarts)
{
    std::string currentLine;

    int currentWidth = 0;

    std::size_t currentLineStart = 0;

    std::size_t start = 0;

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

            currentLineStart =
                start;
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
                currentLine += " ";
                currentLine += word;

                currentWidth =
                    candidateWidth;
            }
            else
            {
                lines.push_back(
                    currentLine
                );

                lineStarts.push_back(
                    currentLineStart
                );

                currentLine =
                    word;

                currentWidth =
                    wordWidth;

                currentLineStart =
                    start;
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

        lineStarts.push_back(
            currentLineStart
        );
    }
}

TTF_Font* readerFontForStyle(
    App& app,
    TextStyle style)
{
    switch (
        style
    )
    {
        case TextStyle::Italic:
        {
            return
                app.fontReaderItalic
                ? app.fontReaderItalic
                : app.fontReader;
        }

        case TextStyle::Heading:
        {
            return
                app.fontHeading
                ? app.fontHeading
                : app.fontReader;
        }

        case TextStyle::Regular:
        default:
        {
            return
                app.fontReader;
        }
    }
}

TTF_Font* smallFontForStyle(
    App& app,
    TextStyle style)
{
    if (
        style ==
            TextStyle::Italic &&
        app.fontSmallItalic
    )
    {
        return
            app.fontSmallItalic;
    }

    return
        app.fontSmall;
}

WrappedTextBlock wrapTextBlock(
    App& app,
    const ContentBlock& block,
    int maxWidth,
    bool smallText = false)
{
    WrappedTextBlock result;

    std::unordered_map<
        std::string,
        int
    > widthCache;

    widthCache.reserve(
        256
    );

    TTF_Font* font =
        smallText
        ? smallFontForStyle(
            app,
            block.style
        )
        : readerFontForStyle(
            app,
            block.style
        );

    int spaceWidth = 0;
    int unusedHeight = 0;

    TTF_SizeUTF8(
        font,
        " ",
        &spaceWidth,
        &unusedHeight
    );

    std::vector<std::string>
        wrappedStrings;

    wrapParagraphFast(
        font,
        block.text,
        maxWidth,
        spaceWidth,
        widthCache,
        wrappedStrings,
        result.lineStarts
    );

    result.lines.reserve(
        wrappedStrings.size()
    );

    for (
        std::string& line :
        wrappedStrings
    )
    {
        TextLine textLine;

        textLine.text =
            std::move(
                line
            );

        textLine.centered =
            block.centered;

        textLine.style =
            block.style;

        result.lines.push_back(
            std::move(
                textLine
            )
        );
    }

    return result;
}

void flushTextPage(
    std::vector<DocumentPage>& pages,
    std::vector<TextLine>& currentLines,
    SemanticPosition& currentPosition)
{
    if (
        currentLines.empty()
    )
    {
        return;
    }

    DocumentPage page;

    page.kind =
        PageKind::Text;

    page.sourcePosition =
        currentPosition;

    page.lines =
        std::move(
            currentLines
        );

    pages.push_back(
        std::move(
            page
        )
    );

    currentLines.clear();

    currentPosition =
        SemanticPosition{};
}

int renderedLineForOffset(
    const std::vector<std::size_t>& lineStarts,
    std::size_t offset)
{
    if (
        lineStarts.empty()
    )
    {
        return 0;
    }

    auto upper =
        std::upper_bound(
            lineStarts.begin(),
            lineStarts.end(),
            offset
        );

    if (
        upper ==
        lineStarts.begin()
    )
    {
        return 0;
    }

    return
        static_cast<int>(
            std::distance(
                lineStarts.begin(),
                upper
            )
            - 1
        );
}

void assignAnchorsForTextBlock(
    const ChapterDocument& document,
    std::size_t blockIndex,
    const std::vector<std::size_t>& lineStarts,
    const std::vector<int>& linePages,
    int fallbackPage,
    std::unordered_map<
        std::string,
        int
    >& anchorPages)
{
    for (
        const auto& pair :
        document.anchors
    )
    {
        const AnchorLocation& location =
            pair.second;

        if (
            location.blockIndex !=
            blockIndex
        )
        {
            continue;
        }

        int page =
            fallbackPage;

        if (
            location.hasTextOffset &&
            !lineStarts.empty() &&
            !linePages.empty()
        )
        {
            int line =
                renderedLineForOffset(
                    lineStarts,
                    location.textOffset
                );

            line =
                std::clamp(
                    line,
                    0,
                    static_cast<int>(
                        linePages.size()
                    )
                    - 1
                );

            page =
                linePages[
                    line
                ];
        }

        anchorPages[
            pair.first
        ] =
            page;
    }
}

void assignAnchorsForWholeBlock(
    const ChapterDocument& document,
    std::size_t blockIndex,
    int page,
    std::unordered_map<
        std::string,
        int
    >& anchorPages)
{
    for (
        const auto& pair :
        document.anchors
    )
    {
        if (
            pair.second.blockIndex ==
            blockIndex
        )
        {
            anchorPages[
                pair.first
            ] =
                page;
        }
    }
}

LayoutResult layoutDocument(
    App& app,
    const ChapterDocument& document)
{
    LayoutResult result;

    std::vector<TextLine>
        currentLines;

    SemanticPosition
        currentPosition;

    const int visibleLines =
        readerVisibleLines(
            app
        );

    const int textWidth =
        readerTextWidth(
            app
        );

    for (
        std::size_t i = 0;
        i < document.blocks.size();
        ++i
    )
    {
        const ContentBlock& block =
            document.blocks[i];

        if (
            block.kind ==
            BlockKind::Text
        )
        {
            WrappedTextBlock wrapped =
                wrapTextBlock(
                    app,
                    block,
                    textWidth
                );

            if (
                !currentLines.empty() &&
                static_cast<int>(
                    currentLines.size()
                ) <
                visibleLines
            )
            {
                currentLines.push_back(
                    TextLine{
                        "",
                        false,
                        TextStyle::Regular
                    }
                );
            }

            if (
                !wrapped.lines.empty() &&
                static_cast<int>(
                    currentLines.size()
                ) >=
                visibleLines
            )
            {
                flushTextPage(
                    result.pages,
                    currentLines,
                    currentPosition
                );
            }

            std::vector<int>
                linePages;

            linePages.reserve(
                wrapped.lines.size()
            );

            for (
                std::size_t lineIndex = 0;
                lineIndex < wrapped.lines.size();
                ++lineIndex
            )
            {
                if (
                    static_cast<int>(
                        currentLines.size()
                    ) >=
                    visibleLines
                )
                {
                    flushTextPage(
                        result.pages,
                        currentLines,
                        currentPosition
                    );
                }

                if (
                    currentLines.empty()
                )
                {
                    currentPosition.blockIndex =
                        i;

                    currentPosition.textOffset =
                        lineIndex <
                            wrapped.lineStarts.size()
                        ? wrapped.lineStarts[
                            lineIndex
                        ]
                        : 0;

                    currentPosition.hasTextOffset =
                        true;

                    currentPosition.valid =
                        true;
                }

                linePages.push_back(
                    static_cast<int>(
                        result.pages.size()
                    )
                );

                currentLines.push_back(
                    wrapped.lines[
                        lineIndex
                    ]
                );
            }

            int fallbackPage =
                static_cast<int>(
                    result.pages.size()
                );

            if (
                !linePages.empty()
            )
            {
                fallbackPage =
                    linePages.front();
            }

            assignAnchorsForTextBlock(
                document,
                i,
                wrapped.lineStarts,
                linePages,
                fallbackPage,
                result.anchorPages
            );

            continue;
        }

        flushTextPage(
            result.pages,
            currentLines,
            currentPosition
        );

        const int imagePageIndex =
            static_cast<int>(
                result.pages.size()
            );

        assignAnchorsForWholeBlock(
            document,
            i,
            imagePageIndex,
            result.anchorPages
        );

        DocumentPage imagePage;

        imagePage.kind =
            PageKind::Image;

        imagePage.sourcePosition.blockIndex =
            i;

        imagePage.sourcePosition.textOffset =
            0;

        imagePage.sourcePosition.hasTextOffset =
            false;

        imagePage.sourcePosition.valid =
            true;

        imagePage.imagePath =
            block.imagePath;

        std::vector<TextLine>
            candidateCaption;

        std::size_t consumedUntil =
            i;

        for (
            std::size_t j = i + 1;
            j < document.blocks.size();
            ++j
        )
        {
            const ContentBlock& next =
                document.blocks[j];

            if (
                next.kind !=
                    BlockKind::Text ||
                !next.centered
            )
            {
                break;
            }

            WrappedTextBlock wrapped =
                wrapTextBlock(
                    app,
                    next,
                    textWidth - 12,
                    true
                );

            if (
                candidateCaption.size()
                    + wrapped.lines.size() >
                MAX_CAPTION_LINES
            )
            {
                break;
            }

            if (
                !candidateCaption.empty()
            )
            {
                candidateCaption.push_back(
                    TextLine{
                        "",
                        true,
                        TextStyle::Regular
                    }
                );
            }

            if (
                candidateCaption.size()
                    + wrapped.lines.size() >
                MAX_CAPTION_LINES
            )
            {
                if (
                    !candidateCaption.empty() &&
                    candidateCaption.back()
                        .text.empty()
                )
                {
                    candidateCaption.pop_back();
                }

                break;
            }

            candidateCaption.insert(
                candidateCaption.end(),
                wrapped.lines.begin(),
                wrapped.lines.end()
            );

            assignAnchorsForWholeBlock(
                document,
                j,
                imagePageIndex,
                result.anchorPages
            );

            consumedUntil =
                j;
        }

        imagePage.captionLines =
            std::move(
                candidateCaption
            );

        result.pages.push_back(
            std::move(
                imagePage
            )
        );

        i =
            consumedUntil;
    }

    flushTextPage(
        result.pages,
        currentLines,
        currentPosition
    );

    if (
        !result.pages.empty()
    )
    {
        const int finalPage =
            static_cast<int>(
                result.pages.size()
            )
            - 1;

        for (
            const auto& pair :
            document.anchors
        )
        {
            if (
                result.anchorPages.find(
                    pair.first
                ) !=
                result.anchorPages.end()
            )
            {
                continue;
            }

            if (
                pair.second.blockIndex >=
                document.blocks.size()
            )
            {
                result.anchorPages[
                    pair.first
                ] =
                    finalPage;
            }
        }
    }

    for (
        const auto& pair :
        result.anchorPages
    )
    {
        debugLog(
            "ANCHOR PAGE: %s -> %d/%zu",
            pair.first.c_str(),
            pair.second + 1,
            result.pages.size()
        );
    }

    return result;
}

bool semanticPositionAtOrBefore(
    const SemanticPosition& candidate,
    const SemanticPosition& target)
{
    if (
        !candidate.valid ||
        !target.valid
    )
    {
        return false;
    }

    if (
        candidate.blockIndex <
        target.blockIndex
    )
    {
        return true;
    }

    if (
        candidate.blockIndex >
        target.blockIndex
    )
    {
        return false;
    }

    return
        candidate.textOffset <=
        target.textOffset;
}

int findPageForSemanticPosition(
    const std::vector<DocumentPage>& pages,
    const SemanticPosition& target)
{
    if (
        pages.empty()
    )
    {
        return 0;
    }

    if (
        !target.valid
    )
    {
        return 0;
    }

    int bestPage =
        0;

    bool found =
        false;

    for (
        std::size_t i = 0;
        i < pages.size();
        ++i
    )
    {
        const SemanticPosition& candidate =
            pages[i].sourcePosition;

        if (
            !semanticPositionAtOrBefore(
                candidate,
                target
            )
        )
        {
            continue;
        }

        bestPage =
            static_cast<int>(
                i
            );

        found =
            true;
    }

    if (
        found
    )
    {
        return bestPage;
    }

    for (
        std::size_t i = 0;
        i < pages.size();
        ++i
    )
    {
        if (
            pages[i].sourcePosition.valid
        )
        {
            return
                static_cast<int>(
                    i
                );
        }
    }

    return 0;
}

void loadSettings(
    App& app)
{
    FILE* file =
        std::fopen(
            SETTINGS_PATH,
            "r"
        );

    if (!file)
        return;

    char buffer[128];

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
            line ==
            "orientation=portrait"
        )
        {
            app.orientation =
                Orientation::Portrait;
        }
        else if (
            line ==
            "orientation=landscape"
        )
        {
            app.orientation =
                Orientation::Landscape;
        }
    }

    std::fclose(
        file
    );
}

void saveSettings(
    const App& app)
{
    FILE* file =
        std::fopen(
            SETTINGS_PATH,
            "w"
        );

    if (!file)
        return;

    std::fprintf(
        file,
        "orientation=%s\n",
        app.orientation ==
            Orientation::Portrait
        ? "portrait"
        : "landscape"
    );

    std::fclose(
        file
    );
}

bool bookPortraitRendering(
    const App& app)
{
    return
        app.orientation ==
            Orientation::Portrait &&
        app.screen !=
            Screen::Library;
}

SDL_Rect rotateLogicalRectClockwise(
    const SDL_Rect& logical)
{
    SDL_Rect physical
    {
        PORTRAIT_HEIGHT
            - (
                logical.y
                + logical.h
            ),

        logical.x,

        logical.h,
        logical.w
    };

    return physical;
}

void fillLogicalRect(
    App& app,
    const SDL_Rect& logical)
{
    SDL_Rect destination =
        bookPortraitRendering(
            app
        )
        ? rotateLogicalRectClockwise(
            logical
        )
        : logical;

    SDL_RenderFillRect(
        app.renderer,
        &destination
    );
}


SDL_Point rotateLogicalPointClockwise(
    const SDL_Point& logical)
{
    SDL_Point physical
    {
        PORTRAIT_HEIGHT
            - 1
            - logical.y,

        logical.x
    };

    return physical;
}

void drawLogicalLine(
    App& app,
    int x1,
    int y1,
    int x2,
    int y2)
{
    SDL_Point first
    {
        x1,
        y1
    };

    SDL_Point second
    {
        x2,
        y2
    };

    if (
        bookPortraitRendering(
            app
        )
    )
    {
        first =
            rotateLogicalPointClockwise(
                first
            );

        second =
            rotateLogicalPointClockwise(
                second
            );
    }

    SDL_RenderDrawLine(
        app.renderer,
        first.x,
        first.y,
        second.x,
        second.y
    );
}

void drawLogicalRectOutline(
    App& app,
    const SDL_Rect& logical)
{
    SDL_Rect destination =
        bookPortraitRendering(
            app
        )
        ? rotateLogicalRectClockwise(
            logical
        )
        : logical;

    SDL_RenderDrawRect(
        app.renderer,
        &destination
    );
}

void drawTriangleButtonIcon(
    App& app,
    int x,
    int y,
    SDL_Color color)
{
    SDL_SetRenderDrawColor(
        app.renderer,
        color.r,
        color.g,
        color.b,
        color.a
    );

    constexpr int size =
        8;

    drawLogicalLine(
        app,
        x + size / 2,
        y,
        x,
        y + size
    );

    drawLogicalLine(
        app,
        x,
        y + size,
        x + size,
        y + size
    );

    drawLogicalLine(
        app,
        x + size,
        y + size,
        x + size / 2,
        y
    );
}

void drawSquareButtonIcon(
    App& app,
    int x,
    int y,
    SDL_Color color)
{
    SDL_SetRenderDrawColor(
        app.renderer,
        color.r,
        color.g,
        color.b,
        color.a
    );

    SDL_Rect logical
    {
        x,
        y,
        9,
        9
    };

    drawLogicalRectOutline(
        app,
        logical
    );
}

void drawCircleButtonIcon(
    App& app,
    int x,
    int y,
    SDL_Color color)
{
    SDL_SetRenderDrawColor(
        app.renderer,
        color.r,
        color.g,
        color.b,
        color.a
    );

    const SDL_Point points[] =
    {
        {x + 3, y},
        {x + 6, y},
        {x + 8, y + 2},
        {x + 9, y + 4},
        {x + 8, y + 7},
        {x + 6, y + 9},
        {x + 3, y + 9},
        {x + 1, y + 7},
        {x,     y + 4},
        {x + 1, y + 2},
        {x + 3, y}
    };

    constexpr std::size_t pointCount =
        sizeof(points)
        /
        sizeof(points[0]);

    for (
        std::size_t i = 0;
        i + 1 < pointCount;
        ++i
    )
    {
        drawLogicalLine(
            app,
            points[i].x,
            points[i].y,
            points[i + 1].x,
            points[i + 1].y
        );
    }
}

void drawText(
    App& app,
    TTF_Font* font,
    const std::string& text,
    int x,
    int y,
    SDL_Color color);

int drawReaderFooterControls(
    App& app,
    int x,
    int y,
    SDL_Color color)
{
    constexpr int iconTextGap =
        4;

    constexpr int itemGap =
        10;

    constexpr int iconWidth =
        9;

    constexpr int iconYOffset =
        3;

    drawTriangleButtonIcon(
        app,
        x,
        y + iconYOffset,
        color
    );

    x +=
        iconWidth
        + iconTextGap;

    drawText(
        app,
        app.fontSmall,
        "Índice",
        x,
        y,
        color
    );

    x +=
        textPixelWidth(
            app.fontSmall,
            "Índice"
        )
        + itemGap;

    drawSquareButtonIcon(
        app,
        x,
        y + iconYOffset,
        color
    );

    x +=
        iconWidth
        + iconTextGap;

    drawText(
        app,
        app.fontSmall,
        "Girar",
        x,
        y,
        color
    );

    x +=
        textPixelWidth(
            app.fontSmall,
            "Girar"
        )
        + itemGap;

    drawCircleButtonIcon(
        app,
        x,
        y + iconYOffset,
        color
    );

    x +=
        iconWidth
        + iconTextGap;

    drawText(
        app,
        app.fontSmall,
        "Volver",
        x,
        y,
        color
    );

    x +=
        textPixelWidth(
            app.fontSmall,
            "Volver"
        );

    return x;
}

SDL_Surface* rotateSurfaceClockwise(
    SDL_Surface* source)
{
    if (
        !source ||
        source->w <= 0 ||
        source->h <= 0
    )
    {
        return nullptr;
    }

    SDL_Surface* rgba =
        SDL_ConvertSurfaceFormat(
            source,
            SDL_PIXELFORMAT_RGBA32,
            0
        );

    if (
        !rgba
    )
    {
        return nullptr;
    }

    SDL_Surface* rotated =
        SDL_CreateRGBSurfaceWithFormat(
            0,
            rgba->h,
            rgba->w,
            32,
            SDL_PIXELFORMAT_RGBA32
        );

    if (
        !rotated
    )
    {
        SDL_FreeSurface(
            rgba
        );

        return nullptr;
    }

    bool sourceLocked = false;
    bool destinationLocked = false;

    if (
        SDL_MUSTLOCK(
            rgba
        )
    )
    {
        if (
            SDL_LockSurface(
                rgba
            ) != 0
        )
        {
            SDL_FreeSurface(
                rotated
            );

            SDL_FreeSurface(
                rgba
            );

            return nullptr;
        }

        sourceLocked =
            true;
    }

    if (
        SDL_MUSTLOCK(
            rotated
        )
    )
    {
        if (
            SDL_LockSurface(
                rotated
            ) != 0
        )
        {
            if (
                sourceLocked
            )
            {
                SDL_UnlockSurface(
                    rgba
                );
            }

            SDL_FreeSurface(
                rotated
            );

            SDL_FreeSurface(
                rgba
            );

            return nullptr;
        }

        destinationLocked =
            true;
    }

    for (
        int y = 0;
        y < rgba->h;
        ++y
    )
    {
        const std::uint32_t* sourceRow =
            reinterpret_cast<
                const std::uint32_t*
            >(
                static_cast<
                    const std::uint8_t*
                >(
                    rgba->pixels
                )
                + y * rgba->pitch
            );

        for (
            int x = 0;
            x < rgba->w;
            ++x
        )
        {
            const int destinationX =
                rgba->h
                - 1
                - y;

            const int destinationY =
                x;

            std::uint32_t* destinationRow =
                reinterpret_cast<
                    std::uint32_t*
                >(
                    static_cast<
                        std::uint8_t*
                    >(
                        rotated->pixels
                    )
                    + destinationY
                        * rotated->pitch
                );

            destinationRow[
                destinationX
            ] =
                sourceRow[
                    x
                ];
        }
    }

    if (
        destinationLocked
    )
    {
        SDL_UnlockSurface(
            rotated
        );
    }

    if (
        sourceLocked
    )
    {
        SDL_UnlockSurface(
            rgba
        );
    }

    SDL_FreeSurface(
        rgba
    );

    return rotated;
}

void resetRendererTransform(
    App& app)
{
    /*
     * Portrait mode is rendered directly to the physical 480x272 PSP
     * framebuffer. Coordinates are rotated mathematically, while text
     * and image surfaces are rotated once before becoming textures.
     *
     * This avoids the PSP SDL renderer clipping an oversized rotated
     * destination during portrait presentation.
     */
    SDL_SetRenderTarget(
        app.renderer,
        nullptr
    );

    SDL_RenderSetLogicalSize(
        app.renderer,
        0,
        0
    );

    SDL_RenderSetScale(
        app.renderer,
        1.0f,
        1.0f
    );

    SDL_RenderSetViewport(
        app.renderer,
        nullptr
    );

    SDL_RenderSetClipRect(
        app.renderer,
        nullptr
    );
}

void beginLandscapeFrame(
    App& app)
{
    resetRendererTransform(
        app
    );
}

bool beginBookFrame(
    App& app)
{
    resetRendererTransform(
        app
    );

    return
        bookPortraitRendering(
            app
        );
}

void presentBookFrame(
    App& app,
    bool portraitFrame)
{
    (void)portraitFrame;

    SDL_RenderPresent(
        app.renderer
    );
}

void destroyReaderImage(
    App& app)
{
    if (
        app.readerImageTexture
    )
    {
        SDL_DestroyTexture(
            app.readerImageTexture
        );

        app.readerImageTexture =
            nullptr;
    }

    app.readerImageWidth = 0;
    app.readerImageHeight = 0;

    app.readerLoadedImagePath.clear();
    app.readerImageError.clear();
}

bool loadImageTextureFromEpub(
    App& app,
    const Book& book,
    const std::string& imagePath,
    int maxWidth,
    int maxHeight,
    SDL_Texture*& outputTexture,
    int& outputWidth,
    int& outputHeight,
    std::string& error)
{
    outputTexture =
        nullptr;

    outputWidth = 0;
    outputHeight = 0;

    int zipError = 0;

    zip_t* archive =
        zip_open(
            book.path.c_str(),
            ZIP_RDONLY,
            &zipError
        );

    if (!archive)
    {
        error =
            "Could not reopen EPUB";

        return false;
    }

    std::vector<std::uint8_t>
        imageBytes;

    if (
        !readZipEntryBytes(
            archive,
            imagePath,
            imageBytes,
            error
        )
    )
    {
        zip_close(
            archive
        );

        return false;
    }

    zip_close(
        archive
    );

    if (
        imageBytes.empty()
    )
    {
        error =
            "Image is empty";

        return false;
    }

    SDL_RWops* rw =
        SDL_RWFromConstMem(
            imageBytes.data(),
            static_cast<int>(
                imageBytes.size()
            )
        );

    if (!rw)
    {
        error =
            SDL_GetError();

        return false;
    }

    SDL_Surface* source =
        IMG_Load_RW(
            rw,
            1
        );

    if (!source)
    {
        error =
            IMG_GetError();

        return false;
    }

    if (
        source->w <= 0 ||
        source->h <= 0
    )
    {
        SDL_FreeSurface(
            source
        );

        error =
            "Invalid image dimensions";

        return false;
    }

    debugLog(
        "IMG: %s source=%dx%d format=%s bpp=%d targetMax=%dx%d",
        imagePath.c_str(),
        source->w,
        source->h,
        SDL_GetPixelFormatName(
            source->format->format
        ),
        source->format->BitsPerPixel,
        maxWidth,
        maxHeight
    );

    const double scaleX =
        static_cast<double>(
            maxWidth
        )
        /
        static_cast<double>(
            source->w
        );

    const double scaleY =
        static_cast<double>(
            maxHeight
        )
        /
        static_cast<double>(
            source->h
        );

    const double scale =
        std::min(
            1.0,
            std::min(
                scaleX,
                scaleY
            )
        );

    const int scaledWidth =
        std::max(
            1,
            static_cast<int>(
                source->w
                * scale
            )
        );

    const int scaledHeight =
        std::max(
            1,
            static_cast<int>(
                source->h
                * scale
            )
        );

    SDL_Surface* finalSurface =
        source;

    SDL_Surface* converted =
        nullptr;

    SDL_Surface* scaled =
        nullptr;

    if (
        scaledWidth !=
            source->w ||
        scaledHeight !=
            source->h
    )
    {
        converted =
            SDL_ConvertSurfaceFormat(
                source,
                SDL_PIXELFORMAT_RGBA32,
                0
            );

        if (
            converted
        )
        {
            scaled =
                SDL_CreateRGBSurfaceWithFormat(
                    0,
                    scaledWidth,
                    scaledHeight,
                    32,
                    SDL_PIXELFORMAT_RGBA32
                );

            if (
                scaled &&
                SDL_BlitScaled(
                    converted,
                    nullptr,
                    scaled,
                    nullptr
                ) == 0
            )
            {
                finalSurface =
                    scaled;
            }
            else
            {
                if (
                    scaled
                )
                {
                    SDL_FreeSurface(
                        scaled
                    );

                    scaled =
                        nullptr;
                }

                debugLog(
                    "Image pre-scale failed, falling back to source texture: %s",
                    SDL_GetError()
                );
            }
        }
        else
        {
            debugLog(
                "Image conversion failed, falling back to source texture: %s",
                SDL_GetError()
            );
        }
    }

    const int logicalWidth =
        finalSurface->w;

    const int logicalHeight =
        finalSurface->h;

    SDL_Surface* textureSurface =
        finalSurface;

    SDL_Surface* rotated =
        nullptr;

    const bool portrait =
        bookPortraitRendering(
            app
        );

    if (
        portrait
    )
    {
        rotated =
            rotateSurfaceClockwise(
                finalSurface
            );

        if (
            !rotated
        )
        {
            error =
                "Could not rotate image";

            if (
                scaled
            )
            {
                SDL_FreeSurface(
                    scaled
                );
            }

            if (
                converted
            )
            {
                SDL_FreeSurface(
                    converted
                );
            }

            SDL_FreeSurface(
                source
            );

            return false;
        }

        textureSurface =
            rotated;
    }

    SDL_Texture* texture =
        SDL_CreateTextureFromSurface(
            app.renderer,
            textureSurface
        );

    if (!texture)
    {
        error =
            SDL_GetError();

        if (
            rotated
        )
        {
            SDL_FreeSurface(
                rotated
            );
        }

        if (
            scaled
        )
        {
            SDL_FreeSurface(
                scaled
            );
        }

        if (
            converted
        )
        {
            SDL_FreeSurface(
                converted
            );
        }

        SDL_FreeSurface(
            source
        );

        return false;
    }

    outputTexture =
        texture;

    /*
     * Keep the logical dimensions. In portrait mode the actual texture
     * has the swapped dimensions because its pixels are already rotated.
     */
    outputWidth =
        logicalWidth;

    outputHeight =
        logicalHeight;

    if (
        rotated
    )
    {
        SDL_FreeSurface(
            rotated
        );
    }

    if (
        scaled
    )
    {
        SDL_FreeSurface(
            scaled
        );
    }

    if (
        converted
    )
    {
        SDL_FreeSurface(
            converted
        );
    }

    SDL_FreeSurface(
        source
    );

    debugLog(
        "IMG texture ready: %s logical=%dx%d texture=%dx%d orientation=%s",
        imagePath.c_str(),
        outputWidth,
        outputHeight,
        portrait
            ? outputHeight
            : outputWidth,
        portrait
            ? outputWidth
            : outputHeight,
        portrait
            ? "portrait"
            : "landscape"
    );

    return true;
}

int imageAreaHeightForPage(
    const App& app,
    const DocumentPage& page)
{
    int captionHeight =
        static_cast<int>(
            page.captionLines.size()
        )
        * 16;

    if (
        !page.captionLines.empty()
    )
    {
        captionHeight += 8;
    }

    return
        std::max(
            80,
            (
                readerContentBottom(
                    app
                )
                - 43
            )
            - captionHeight
        );
}

void loadCurrentPageImage(
    App& app)
{
    destroyReaderImage(
        app
    );

    if (
        app.readerPageIndex < 0 ||
        app.readerPageIndex >=
            static_cast<int>(
                app.readerPages.size()
            )
    )
    {
        return;
    }

    const DocumentPage& page =
        app.readerPages[
            app.readerPageIndex
        ];

    if (
        page.kind !=
        PageKind::Image
    )
    {
        return;
    }

    if (
        app.currentBookIndex < 0 ||
        app.currentBookIndex >=
            static_cast<int>(
                app.books.size()
            )
    )
    {
        app.readerImageError =
            "Invalid book";

        return;
    }

    const int maxHeight =
        imageAreaHeightForPage(
            app,
            page
        );

    std::string error;

    SDL_Texture* texture =
        nullptr;

    int width = 0;
    int height = 0;

    if (
        !loadImageTextureFromEpub(
            app,
            app.books[
                app.currentBookIndex
            ],
            page.imagePath,
            readerTextWidth(
                app
            ),
            maxHeight,
            texture,
            width,
            height,
            error
        )
    )
    {
        app.readerImageError =
            error;

        debugLog(
            "Page image failed: %s -> %s",
            page.imagePath.c_str(),
            error.c_str()
        );

        return;
    }

    app.readerImageTexture =
        texture;

    app.readerImageWidth =
        width;

    app.readerImageHeight =
        height;

    app.readerLoadedImagePath =
        page.imagePath;
}

SemanticPosition currentReaderSemanticPosition(
    const App& app)
{
    /*
     * readerSemanticPosition is a stable reading anchor.
     *
     * After a semantic restore the exact anchor can be inside the new
     * page instead of at its first rendered line. Replacing the anchor
     * with that page's top on every relayout causes cumulative backwards
     * drift when repeatedly toggling Landscape <-> Portrait.
     *
     * Keep the exact anchor until the user explicitly changes page.
     */
    if (
        app.readerSemanticPosition.valid
    )
    {
        return
            app.readerSemanticPosition;
    }

    if (
        app.readerPageIndex < 0 ||
        app.readerPageIndex >=
            static_cast<int>(
                app.readerPages.size()
            )
    )
    {
        return
            SemanticPosition{};
    }

    return
        app.readerPages[
            app.readerPageIndex
        ].sourcePosition;
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

    bool version2 = false;
    bool version3 = false;
    bool firstMeaningfulLine = true;

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

        if (
            firstMeaningfulLine &&
            line ==
                "#LARP_PROGRESS_V3"
        )
        {
            version3 =
                true;

            firstMeaningfulLine =
                false;

            continue;
        }

        if (
            firstMeaningfulLine &&
            line ==
                "#LARP_PROGRESS_V2"
        )
        {
            version2 =
                true;

            firstMeaningfulLine =
                false;

            continue;
        }

        firstMeaningfulLine =
            false;

        if (
            !line.empty() &&
            line[0] == '#'
        )
        {
            continue;
        }

        std::vector<std::string>
            fields;

        std::size_t start = 0;

        while (
            start <=
            line.size()
        )
        {
            const std::size_t tab =
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
            fields.size() < 2
        )
        {
            continue;
        }

        ReadingProgress progress;

        if (
            version3 &&
            fields.size() >= 7
        )
        {
            progress.spineIndex =
                std::max(
                    0,
                    std::atoi(
                        fields[1].c_str()
                    )
                );

            progress.pageIndex =
                std::max(
                    0,
                    std::atoi(
                        fields[2].c_str()
                    )
                );

            progress.semanticPosition.blockIndex =
                static_cast<std::size_t>(
                    std::strtoull(
                        fields[3].c_str(),
                        nullptr,
                        10
                    )
                );

            progress.semanticPosition.textOffset =
                static_cast<std::size_t>(
                    std::strtoull(
                        fields[4].c_str(),
                        nullptr,
                        10
                    )
                );

            progress.semanticPosition.hasTextOffset =
                std::atoi(
                    fields[5].c_str()
                ) != 0;

            progress.semanticPosition.valid =
                std::atoi(
                    fields[6].c_str()
                ) != 0;
        }
        else if (
            fields.size() == 2
        )
        {
            progress.spineIndex =
                0;

            const int oldTopLine =
                std::max(
                    0,
                    std::atoi(
                        fields[1].c_str()
                    )
                );

            progress.pageIndex =
                oldTopLine
                /
                LEGACY_READER_VISIBLE_LINES;
        }
        else
        {
            progress.spineIndex =
                std::max(
                    0,
                    std::atoi(
                        fields[1].c_str()
                    )
                );

            const int rawPosition =
                std::max(
                    0,
                    std::atoi(
                        fields[2].c_str()
                    )
                );

            progress.pageIndex =
                version2
                ? rawPosition
                : rawPosition
                    /
                    LEGACY_READER_VISIBLE_LINES;
        }

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
        return;

    std::fprintf(
        file,
        "#LARP_PROGRESS_V3\n"
    );

    for (
        const auto& entry :
        app.savedProgress
    )
    {
        const SemanticPosition& semantic =
            entry.second.semanticPosition;

        std::fprintf(
            file,
            "%s\t%d\t%d\t%llu\t%llu\t%d\t%d\n",
            entry.first.c_str(),
            entry.second.spineIndex,
            entry.second.pageIndex,
            static_cast<unsigned long long>(
                semantic.blockIndex
            ),
            static_cast<unsigned long long>(
                semantic.textOffset
            ),
            semantic.hasTextOffset
                ? 1
                : 0,
            semantic.valid
                ? 1
                : 0
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
        app.readerSpineIndex < 0
    )
    {
        return;
    }

    ReadingProgress progress;

    progress.spineIndex =
        app.readerSpineIndex;

    progress.pageIndex =
        std::max(
            0,
            app.readerPageIndex
        );

    progress.semanticPosition =
        currentReaderSemanticPosition(
            app
        );

    app.savedProgress[
        app.readerBookFilename
    ] =
        progress;

    saveProgress(
        app
    );

    if (
        progress.semanticPosition.valid
    )
    {
        debugLog(
            "PROGRESS SAVE: spine=%d page=%d block=%llu offset=%llu text=%s",
            progress.spineIndex + 1,
            progress.pageIndex + 1,
            static_cast<unsigned long long>(
                progress.semanticPosition
                    .blockIndex
            ),
            static_cast<unsigned long long>(
                progress.semanticPosition
                    .textOffset
            ),
            progress.semanticPosition
                    .hasTextOffset
                ? "yes"
                : "no"
        );
    }
}

std::string makeTextCacheKey(
    TTF_Font* font,
    const std::string& text,
    SDL_Color color,
    bool portrait)
{
    return
        std::to_string(
            reinterpret_cast<
                std::uintptr_t
            >(
                font
            )
        )
        + "|"
        + (
            portrait
            ? "P"
            : "L"
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
    const bool portrait =
        bookPortraitRendering(
            app
        );

    const std::string key =
        makeTextCacheKey(
            font,
            text,
            color,
            portrait
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

    const int logicalWidth =
        surface->w;

    const int logicalHeight =
        surface->h;

    SDL_Surface* textureSurface =
        surface;

    SDL_Surface* rotated =
        nullptr;

    if (
        portrait
    )
    {
        rotated =
            rotateSurfaceClockwise(
                surface
            );

        if (
            !rotated
        )
        {
            debugLog(
                "Text rotation failed: %s",
                SDL_GetError()
            );

            SDL_FreeSurface(
                surface
            );

            return nullptr;
        }

        textureSurface =
            rotated;
    }

    SDL_Texture* texture =
        SDL_CreateTextureFromSurface(
            app.renderer,
            textureSurface
        );

    if (!texture)
    {
        if (
            rotated
        )
        {
            SDL_FreeSurface(
                rotated
            );
        }

        SDL_FreeSurface(
            surface
        );

        return nullptr;
    }

    CachedText cached;

    cached.texture =
        texture;

    /*
     * These remain the logical, unrotated dimensions. The backing
     * portrait texture is logicalHeight x logicalWidth.
     */
    cached.width =
        logicalWidth;

    cached.height =
        logicalHeight;

    if (
        rotated
    )
    {
        SDL_FreeSurface(
            rotated
        );
    }

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

    SDL_Rect logical
    {
        x,
        y,
        cached->width,
        cached->height
    };

    SDL_Rect destination =
        bookPortraitRendering(
            app
        )
        ? rotateLogicalRectClockwise(
            logical
        )
        : logical;

    SDL_RenderCopy(
        app.renderer,
        cached->texture,
        nullptr,
        &destination
    );
}

void drawCenteredText(
    App& app,
    TTF_Font* font,
    const std::string& text,
    int y,
    SDL_Color color)
{
    if (
        text.empty()
    )
    {
        return;
    }

    const int width =
        textPixelWidth(
            font,
            text
        );

    drawText(
        app,
        font,
        text,
        std::max(
            readerTextLeft(
                app
            ),
            (
                canvasWidth(
                    app
                )
                - width
            )
            / 2
        ),
        y,
        color
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

int activeTocIndexForSpine(
    const App& app,
    int spineIndex)
{
    int previous =
        -1;

    for (
        std::size_t i = 0;
        i < app.readerToc.size();
        ++i
    )
    {
        const int entrySpine =
            app.readerToc[i].spineIndex;

        if (
            entrySpine ==
            spineIndex
        )
        {
            return
                static_cast<int>(
                    i
                );
        }

        if (
            entrySpine <
            spineIndex
        )
        {
            previous =
                static_cast<int>(
                    i
                );

            continue;
        }

        break;
    }

    return previous;
}

std::string currentSectionTitle(
    const App& app)
{
    if (
        app.readerActiveTocIndex >= 0 &&
        app.readerActiveTocIndex <
            static_cast<int>(
                app.readerToc.size()
            )
    )
    {
        const TocEntry& entry =
            app.readerToc[
                app.readerActiveTocIndex
            ];

        const std::string lowerTitle =
            toLower(
                entry.title
            );

        const bool coverLike =
            lowerTitle.find(
                "cover"
            ) !=
                std::string::npos ||
            lowerTitle.find(
                "cubierta"
            ) !=
                std::string::npos;

        if (
            !(
                coverLike &&
                app.readerSpineIndex >
                    entry.spineIndex
            )
        )
        {
            return
                entry.title;
        }
    }

    if (
        app.readerSpineIndex >= 0 &&
        app.readerSpineIndex <
            static_cast<int>(
                app.readerSpine.size()
            )
    )
    {
        return
            app.readerSpine[
                app.readerSpineIndex
            ].title;
    }

    return
        app.readerTitle;
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

    debugLog(
        "ERROR: %s",
        error.c_str()
    );
}

void scheduleDocumentLoad(
    App& app,
    int spineIndex,
    DocumentPosition position,
    int restorePage,
    int searchDirection,
    Screen returnScreen,
    const std::string& fragment,
    int tocIndex,
    SemanticPosition semanticPosition =
        SemanticPosition{})
{
    if (
        spineIndex < 0 ||
        spineIndex >=
            static_cast<int>(
                app.readerSpine.size()
            )
    )
    {
        return;
    }

    app.pendingSpineIndex =
        spineIndex;

    app.pendingPosition =
        position;

    app.pendingRestorePage =
        restorePage;

    app.pendingSemanticPosition =
        semanticPosition;

    app.pendingSearchDirection =
        searchDirection >= 0
        ? 1
        : -1;

    app.pendingFragment =
        fragment;

    app.pendingTocIndex =
        tocIndex;

    app.pendingDocument =
        ChapterDocument{};

    app.loadingStage =
        LoadingStage::LoadDocument;

    app.loadingReturnScreen =
        returnScreen;

    app.loadingMessage =
        "Loading section...";

    if (
        tocIndex >= 0 &&
        tocIndex <
            static_cast<int>(
                app.readerToc.size()
            )
    )
    {
        app.loadingDetail =
            app.readerToc[
                tocIndex
            ].title;
    }
    else
    {
        app.loadingDetail =
            app.readerSpine[
                spineIndex
            ].title;
    }

    app.loadingWarmupFrames =
        0;

    app.screen =
        Screen::Loading;

    clearTextCache(
        app
    );
}

void finishSearchWithoutResult(
    App& app)
{
    if (
        app.loadingReturnScreen ==
            Screen::Reader &&
        !app.readerPages.empty() &&
        app.readerSpineIndex >= 0
    )
    {
        app.pendingDocument =
            ChapterDocument{};

        app.pendingSpineIndex =
            -1;

        app.pendingTocIndex =
            -1;

        app.pendingFragment.clear();

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

    loadingFailed(
        app,
        "No supported reading content found"
    );
}

void skipPendingSpineItem(
    App& app,
    const std::string& reason)
{
    debugLog(
        "Skipping spine item %d: %s",
        app.pendingSpineIndex + 1,
        reason.c_str()
    );

    const int nextIndex =
        app.pendingSpineIndex
        + app.pendingSearchDirection;

    if (
        nextIndex < 0 ||
        nextIndex >=
            static_cast<int>(
                app.readerSpine.size()
            )
    )
    {
        finishSearchWithoutResult(
            app
        );

        return;
    }

    scheduleDocumentLoad(
        app,
        nextIndex,
        app.pendingSearchDirection > 0
            ? DocumentPosition::Start
            : DocumentPosition::End,
        0,
        app.pendingSearchDirection,
        app.loadingReturnScreen,
        "",
        -1
    );
}

void beginOpenSelectedBook(
    App& app)
{
    if (
        app.books.empty() ||
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

    if (
        !book.valid
    )
    {
        return;
    }

    destroyReaderImage(
        app
    );

    clearTextCache(
        app
    );

    app.currentBookIndex =
        app.selected;

    app.readerBookFilename.clear();
    app.readerTitle.clear();

    app.readerSpine.clear();
    app.readerToc.clear();
    app.readerPages.clear();

    app.readerSpineIndex =
        -1;

    app.readerPageIndex =
        0;

    app.readerSemanticPosition =
        SemanticPosition{};

    app.readerActiveTocIndex =
        -1;

    app.tocSelected =
        0;

    app.pendingFragment.clear();

    app.pendingTocIndex =
        -1;

    app.loadingStage =
        LoadingStage::OpenStructure;

    app.loadingReturnScreen =
        Screen::Library;

    app.loadingMessage =
        "Opening EPUB...";

    app.loadingDetail =
        "Reading spine and table of contents";

    app.loadingWarmupFrames =
        0;

    app.screen =
        Screen::Loading;
}

void finishDocument(
    App& app,
    LayoutResult&& layout)
{
    if (
        layout.pages.empty()
    )
    {
        skipPendingSpineItem(
            app,
            "Document produced no pages"
        );

        return;
    }

    const bool semanticRelayout =
        app.pendingPosition ==
        DocumentPosition::Semantic;

    const SemanticPosition
        requestedSemanticPosition =
            app.pendingSemanticPosition;

    app.readerPages =
        std::move(
            layout.pages
        );

    app.readerSpineIndex =
        app.pendingSpineIndex;

    if (
        app.pendingTocIndex >= 0 &&
        app.pendingTocIndex <
            static_cast<int>(
                app.readerToc.size()
            )
    )
    {
        app.readerActiveTocIndex =
            app.pendingTocIndex;
    }
    else
    {
        app.readerActiveTocIndex =
            activeTocIndexForSpine(
                app,
                app.readerSpineIndex
            );
    }

    switch (
        app.pendingPosition
    )
    {
        case DocumentPosition::Start:
        {
            app.readerPageIndex =
                0;

            if (
                !app.pendingFragment.empty()
            )
            {
                auto anchor =
                    layout.anchorPages.find(
                        app.pendingFragment
                    );

                if (
                    anchor !=
                    layout.anchorPages.end()
                )
                {
                    app.readerPageIndex =
                        std::clamp(
                            anchor->second,
                            0,
                            static_cast<int>(
                                app.readerPages.size()
                            )
                            - 1
                        );

                    debugLog(
                        "TOC ANCHOR JUMP OK: #%s -> page %d/%zu",
                        app.pendingFragment.c_str(),
                        app.readerPageIndex + 1,
                        app.readerPages.size()
                    );
                }
                else
                {
                    debugLog(
                        "TOC ANCHOR NOT FOUND: #%s -> chapter start",
                        app.pendingFragment.c_str()
                    );
                }
            }

            break;
        }

        case DocumentPosition::End:
        {
            app.readerPageIndex =
                static_cast<int>(
                    app.readerPages.size()
                )
                - 1;

            break;
        }

        case DocumentPosition::Restore:
        {
            app.readerPageIndex =
                std::clamp(
                    app.pendingRestorePage,
                    0,
                    static_cast<int>(
                        app.readerPages.size()
                    )
                    - 1
                );

            break;
        }

        case DocumentPosition::Semantic:
        {
            app.readerPageIndex =
                std::clamp(
                    findPageForSemanticPosition(
                        app.readerPages,
                        app.pendingSemanticPosition
                    ),
                    0,
                    static_cast<int>(
                        app.readerPages.size()
                    )
                    - 1
                );

            debugLog(
                "SEMANTIC RESTORE: block=%llu offset=%llu text=%s -> page %d/%zu",
                static_cast<unsigned long long>(
                    app.pendingSemanticPosition
                        .blockIndex
                ),
                static_cast<unsigned long long>(
                    app.pendingSemanticPosition
                        .textOffset
                ),
                app.pendingSemanticPosition
                        .hasTextOffset
                    ? "yes"
                    : "no",
                app.readerPageIndex + 1,
                app.readerPages.size()
            );

            break;
        }
    }

    if (
        semanticRelayout &&
        requestedSemanticPosition.valid
    )
    {
        /*
         * Keep the exact requested semantic anchor. The restored page may
         * begin earlier than the anchor, but using that earlier page start
         * as the next anchor is what caused the cumulative drift.
         */
        app.readerSemanticPosition =
            requestedSemanticPosition;

        const SemanticPosition& pageTop =
            app.readerPages[
                app.readerPageIndex
            ].sourcePosition;

        debugLog(
            "SEMANTIC ANCHOR KEPT: block=%llu offset=%llu text=%s pageTopBlock=%llu pageTopOffset=%llu",
            static_cast<unsigned long long>(
                app.readerSemanticPosition
                    .blockIndex
            ),
            static_cast<unsigned long long>(
                app.readerSemanticPosition
                    .textOffset
            ),
            app.readerSemanticPosition
                    .hasTextOffset
                ? "yes"
                : "no",
            static_cast<unsigned long long>(
                pageTop.blockIndex
            ),
            static_cast<unsigned long long>(
                pageTop.textOffset
            )
        );
    }
    else if (
        app.readerPageIndex >= 0 &&
        app.readerPageIndex <
            static_cast<int>(
                app.readerPages.size()
            )
    )
    {
        app.readerSemanticPosition =
            app.readerPages[
                app.readerPageIndex
            ].sourcePosition;
    }
    else
    {
        app.readerSemanticPosition =
            SemanticPosition{};
    }

    app.tocSelected =
        app.readerActiveTocIndex >= 0
        ? app.readerActiveTocIndex
        : 0;

    app.pendingDocument =
        ChapterDocument{};

    app.pendingSpineIndex =
        -1;

    app.pendingTocIndex =
        -1;

    app.pendingFragment.clear();

    app.pendingSemanticPosition =
        SemanticPosition{};

    app.loadingStage =
        LoadingStage::None;

    app.loadingMessage.clear();
    app.loadingDetail.clear();

    app.screen =
        Screen::Reader;

    clearTextCache(
        app
    );

    loadCurrentPageImage(
        app
    );

    debugLog(
        "DISPLAY: orientation=%s spine=%d/%zu page=%d/%zu title=%s",
        app.orientation ==
            Orientation::Portrait
        ? "portrait"
        : "landscape",
        app.readerSpineIndex + 1,
        app.readerSpine.size(),
        app.readerPageIndex + 1,
        app.readerPages.size(),
        currentSectionTitle(
            app
        ).c_str()
    );

    if (
        semanticRelayout
    )
    {
        rememberCurrentProgress(
            app
        );
    }
}

void processLoadingStage(
    App& app)
{
    if (
        app.screen !=
            Screen::Loading ||
        app.loadingStage ==
            LoadingStage::Error
    )
    {
        return;
    }

    if (
        app.loadingWarmupFrames < 2
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
            app.currentBookIndex < 0 ||
            app.currentBookIndex >=
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
                app.currentBookIndex
            ];

        ReaderStructure structure =
            loadReaderStructure(
                book
            );

        if (
            !structure.valid
        )
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

        app.readerSpine =
            std::move(
                structure.spine
            );

        app.readerToc =
            std::move(
                structure.toc
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

        progress.spineIndex =
            std::clamp(
                progress.spineIndex,
                0,
                static_cast<int>(
                    app.readerSpine.size()
                )
                - 1
            );

        scheduleDocumentLoad(
            app,
            progress.spineIndex,
            progress.semanticPosition.valid
                ? DocumentPosition::Semantic
                : DocumentPosition::Restore,
            progress.pageIndex,
            1,
            Screen::Library,
            "",
            -1,
            progress.semanticPosition
        );

        return;
    }

    if (
        app.loadingStage ==
        LoadingStage::LoadDocument
    )
    {
        if (
            app.currentBookIndex < 0 ||
            app.currentBookIndex >=
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
            app.pendingSpineIndex < 0 ||
            app.pendingSpineIndex >=
                static_cast<int>(
                    app.readerSpine.size()
                )
        )
        {
            loadingFailed(
                app,
                "Invalid spine index"
            );

            return;
        }

        const Book& book =
            app.books[
                app.currentBookIndex
            ];

        ChapterDocument document =
            loadChapterDocument(
                book,
                app.readerSpine[
                    app.pendingSpineIndex
                ].path
            );

        if (
            !document.valid
        )
        {
            if (
                document.skippable
            )
            {
                skipPendingSpineItem(
                    app,
                    document.error
                );

                return;
            }

            loadingFailed(
                app,
                document.error
            );

            return;
        }

        app.pendingDocument =
            std::move(
                document
            );

        app.loadingStage =
            LoadingStage::LayoutDocument;

        app.loadingMessage =
            "Preparing pages...";

        app.loadingDetail =
            app.pendingTocIndex >= 0 &&
            app.pendingTocIndex <
                static_cast<int>(
                    app.readerToc.size()
                )
            ? app.readerToc[
                app.pendingTocIndex
            ].title
            : app.readerSpine[
                app.pendingSpineIndex
            ].title;

        app.loadingWarmupFrames =
            0;

        return;
    }

    if (
        app.loadingStage ==
        LoadingStage::LayoutDocument
    )
    {
        LayoutResult layout =
            layoutDocument(
                app,
                app.pendingDocument
            );

        debugLog(
            "LAYOUT: orientation=%s %s -> %zu pages, %zu mapped anchors",
            app.orientation ==
                Orientation::Portrait
            ? "portrait"
            : "landscape",
            app.pendingDocument
                .path.c_str(),
            layout.pages.size(),
            layout.anchorPages.size()
        );

        finishDocument(
            app,
            std::move(
                layout
            )
        );
    }
}

void closeReader(
    App& app)
{
    rememberCurrentProgress(
        app
    );

    destroyReaderImage(
        app
    );

    clearTextCache(
        app
    );

    app.screen =
        Screen::Library;

    app.currentBookIndex =
        -1;

    app.readerBookFilename.clear();
    app.readerTitle.clear();

    app.readerSpine.clear();
    app.readerToc.clear();
    app.readerPages.clear();

    app.readerSpineIndex =
        -1;

    app.readerPageIndex =
        0;

    app.readerSemanticPosition =
        SemanticPosition{};

    app.readerActiveTocIndex =
        -1;

    app.tocSelected =
        0;

    app.pendingSpineIndex =
        -1;

    app.pendingTocIndex =
        -1;

    app.pendingFragment.clear();

    app.pendingDocument =
        ChapterDocument{};
}

void navigateToAdjacentDocument(
    App& app,
    int direction)
{
    const int target =
        app.readerSpineIndex
        + direction;

    if (
        target < 0 ||
        target >=
            static_cast<int>(
                app.readerSpine.size()
            )
    )
    {
        return;
    }

    rememberCurrentProgress(
        app
    );

    scheduleDocumentLoad(
        app,
        target,
        direction > 0
            ? DocumentPosition::Start
            : DocumentPosition::End,
        0,
        direction,
        Screen::Reader,
        "",
        -1
    );
}

void setReaderPage(
    App& app,
    int newPage)
{
    if (
        newPage < 0 ||
        newPage >=
            static_cast<int>(
                app.readerPages.size()
            )
    )
    {
        return;
    }

    app.readerPageIndex =
        newPage;

    app.readerSemanticPosition =
        app.readerPages[
            app.readerPageIndex
        ].sourcePosition;

    clearTextCache(
        app
    );

    loadCurrentPageImage(
        app
    );
}

void moveReaderByPage(
    App& app,
    int direction)
{
    if (
        app.readerPages.empty()
    )
    {
        return;
    }

    const int target =
        app.readerPageIndex
        + direction;

    if (
        target >= 0 &&
        target <
            static_cast<int>(
                app.readerPages.size()
            )
    )
    {
        setReaderPage(
            app,
            target
        );

        rememberCurrentProgress(
            app
        );

        return;
    }

    navigateToAdjacentDocument(
        app,
        direction
    );
}

void toggleReaderOrientation(
    App& app)
{
    if (
        app.readerSpineIndex < 0 ||
        app.readerPages.empty()
    )
    {
        return;
    }

    const SemanticPosition semanticPosition =
        currentReaderSemanticPosition(
            app
        );

    const Orientation next =
        app.orientation ==
            Orientation::Landscape
        ? Orientation::Portrait
        : Orientation::Landscape;

    app.orientation =
        next;

    saveSettings(
        app
    );

    if (
        semanticPosition.valid
    )
    {
        debugLog(
            "Orientation changed to %s at semantic block=%llu offset=%llu text=%s",
            app.orientation ==
                Orientation::Portrait
            ? "portrait"
            : "landscape",
            static_cast<unsigned long long>(
                semanticPosition.blockIndex
            ),
            static_cast<unsigned long long>(
                semanticPosition.textOffset
            ),
            semanticPosition.hasTextOffset
                ? "yes"
                : "no"
        );
    }
    else
    {
        debugLog(
            "Orientation changed to %s without semantic position",
            app.orientation ==
                Orientation::Portrait
            ? "portrait"
            : "landscape"
        );
    }

    destroyReaderImage(
        app
    );

    scheduleDocumentLoad(
        app,
        app.readerSpineIndex,
        semanticPosition.valid
            ? DocumentPosition::Semantic
            : DocumentPosition::Restore,
        app.readerPageIndex,
        1,
        Screen::Reader,
        "",
        app.readerActiveTocIndex,
        semanticPosition
    );
}

void openToc(
    App& app)
{
    if (
        app.readerToc.empty()
    )
    {
        return;
    }

    if (
        app.readerActiveTocIndex < 0 ||
        app.readerActiveTocIndex >=
            static_cast<int>(
                app.readerToc.size()
            )
    )
    {
        app.readerActiveTocIndex =
            activeTocIndexForSpine(
                app,
                app.readerSpineIndex
            );
    }

    app.tocSelected =
        app.readerActiveTocIndex >= 0
        ? app.readerActiveTocIndex
        : 0;

    app.screen =
        Screen::Toc;

    clearTextCache(
        app
    );
}

void closeToc(
    App& app)
{
    app.screen =
        Screen::Reader;

    clearTextCache(
        app
    );
}

void jumpToTocSelection(
    App& app)
{
    if (
        app.tocSelected < 0 ||
        app.tocSelected >=
            static_cast<int>(
                app.readerToc.size()
            )
    )
    {
        return;
    }

    const TocEntry& target =
        app.readerToc[
            app.tocSelected
        ];

    if (
        target.spineIndex < 0
    )
    {
        return;
    }

    rememberCurrentProgress(
        app
    );

    int direction = 1;

    if (
        target.spineIndex <
        app.readerSpineIndex
    )
    {
        direction =
            -1;
    }
    else if (
        target.spineIndex ==
            app.readerSpineIndex &&
        app.readerActiveTocIndex >= 0 &&
        app.tocSelected <
            app.readerActiveTocIndex
    )
    {
        direction =
            -1;
    }

    debugLog(
        "TOC JUMP: toc=%d spine=%d path=%s fragment=%s title=%s",
        app.tocSelected + 1,
        target.spineIndex + 1,
        target.path.c_str(),
        target.fragment.empty()
            ? "(none)"
            : target.fragment.c_str(),
        target.title.c_str()
    );

    scheduleDocumentLoad(
        app,
        target.spineIndex,
        DocumentPosition::Start,
        0,
        direction,
        Screen::Reader,
        target.fragment,
        app.tocSelected
    );
}

void cancelLoading(
    App& app)
{
    app.loadingStage =
        LoadingStage::None;

    app.loadingMessage.clear();
    app.loadingDetail.clear();

    app.pendingDocument =
        ChapterDocument{};

    app.pendingSpineIndex =
        -1;

    app.pendingTocIndex =
        -1;

    app.pendingFragment.clear();

    if (
        app.loadingReturnScreen ==
            Screen::Reader &&
        !app.readerPages.empty()
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
        return false;
    }

    resetDebugLog();

    debugLog(
        "SDL initialized"
    );

    const int imageFlags =
        IMG_INIT_PNG |
        IMG_INIT_JPG;

    const int initializedImageFlags =
        IMG_Init(
            imageFlags
        );

    debugLog(
        "IMG_Init requested=%d result=%d",
        imageFlags,
        initializedImageFlags
    );

    if (
        TTF_Init() != 0
    )
    {
        debugLog(
            "TTF_Init failed: %s",
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

    resetRendererTransform(
        app
    );

    loadSettings(
        app
    );

    app.fontNormal =
        loadFont(
            17
        );

    app.fontSmall =
        loadFont(
            12
        );

    app.fontSmallItalic =
        loadFont(
            12
        );

    app.fontReader =
        loadFont(
            14
        );

    app.fontReaderItalic =
        loadFont(
            14
        );

    app.fontHeading =
        loadFont(
            16
        );

    if (
        app.fontSmallItalic
    )
    {
        TTF_SetFontStyle(
            app.fontSmallItalic,
            TTF_STYLE_ITALIC
        );
    }

    if (
        app.fontReaderItalic
    )
    {
        TTF_SetFontStyle(
            app.fontReaderItalic,
            TTF_STYLE_ITALIC
        );
    }

    if (
        app.fontHeading
    )
    {
        TTF_SetFontStyle(
            app.fontHeading,
            TTF_STYLE_BOLD
        );
    }

    if (
        !app.fontNormal ||
        !app.fontSmall ||
        !app.fontSmallItalic ||
        !app.fontReader ||
        !app.fontReaderItalic ||
        !app.fontHeading
    )
    {
        debugLog(
            "Font load failed: %s",
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

    debugLog(
        "Found %zu EPUB(s)",
        app.books.size()
    );

    debugLog(
        "Startup orientation: %s",
        app.orientation ==
            Orientation::Portrait
        ? "portrait"
        : "landscape"
    );

    return true;
}

void handleLibraryButton(
    App& app,
    Uint8 button)
{
    switch (
        button
    )
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
        rememberCurrentProgress(
            app
        );

        app.running =
            false;
    }
}

void handleReaderButton(
    App& app,
    Uint8 button)
{
    switch (
        button
    )
    {
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

        case SDL_CONTROLLER_BUTTON_DPAD_UP:
        {
            moveReaderByPage(
                app,
                -1
            );

            break;
        }

        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        {
            moveReaderByPage(
                app,
                1
            );

            break;
        }

        case SDL_CONTROLLER_BUTTON_Y:
        {
            openToc(
                app
            );

            break;
        }

        case SDL_CONTROLLER_BUTTON_X:
        {
            toggleReaderOrientation(
                app
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

void handleTocButton(
    App& app,
    Uint8 button)
{
    switch (
        button
    )
    {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
        {
            if (
                !app.readerToc.empty()
            )
            {
                --app.tocSelected;

                if (
                    app.tocSelected < 0
                )
                {
                    app.tocSelected =
                        static_cast<int>(
                            app.readerToc.size()
                        )
                        - 1;
                }
            }

            break;
        }

        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        {
            if (
                !app.readerToc.empty()
            )
            {
                ++app.tocSelected;

                if (
                    app.tocSelected >=
                    static_cast<int>(
                        app.readerToc.size()
                    )
                )
                {
                    app.tocSelected =
                        0;
                }
            }

            break;
        }

        case SDL_CONTROLLER_BUTTON_A:
        {
            jumpToTocSelection(
                app
            );

            break;
        }

        case SDL_CONTROLLER_BUTTON_B:
        case SDL_CONTROLLER_BUTTON_Y:
        {
            closeToc(
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
            rememberCurrentProgress(
                app
            );

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

            case Screen::Toc:
            {
                handleTocButton(
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
    beginLandscapeFrame(
        app
    );

    const SDL_Color background
    {
        18,
        19,
        20,
        255
    };

    const SDL_Color panel
    {
        31,
        33,
        35,
        255
    };

    const SDL_Color text
    {
        235,
        235,
        230,
        255
    };

    const SDL_Color muted
    {
        150,
        150,
        145,
        255
    };

    const SDL_Color selectedText
    {
        255,
        255,
        240,
        255
    };

    const SDL_Color errorText
    {
        210,
        130,
        130,
        255
    };

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

        const int firstBook =
            app.selected >=
                visibleBooks
            ? app.selected
                - visibleBooks
                + 1
            : 0;

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
                fitTextToWidth(
                    app.fontNormal,
                    title,
                    430
                ),
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
                fitTextToWidth(
                    app.fontSmall,
                    subtitle,
                    430
                ),
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
    const bool portraitFrame =
        beginBookFrame(
            app
        );

    const int width =
        canvasWidth(
            app
        );

    const int height =
        canvasHeight(
            app
        );

    const SDL_Color background
    {
        18,
        19,
        20,
        255
    };

    const SDL_Color panel
    {
        31,
        33,
        35,
        255
    };

    const SDL_Color text
    {
        235,
        235,
        230,
        255
    };

    const SDL_Color muted
    {
        150,
        150,
        145,
        255
    };

    const SDL_Color error
    {
        220,
        140,
        140,
        255
    };

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
        width,
        42
    };

    SDL_SetRenderDrawColor(
        app.renderer,
        panel.r,
        panel.g,
        panel.b,
        panel.a
    );

    fillLogicalRect(
        app,
        header
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
        app.currentBookIndex >= 0 &&
        app.currentBookIndex <
            static_cast<int>(
                app.books.size()
            )
    )
    {
        bookTitle =
            app.books[
                app.currentBookIndex
            ].title;
    }

    drawText(
        app,
        app.fontSmall,
        fitTextToWidth(
            app.fontSmall,
            bookTitle,
            width - 48
        ),
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
        fitTextToWidth(
            app.fontNormal,
            app.loadingMessage,
            width - 48
        ),
        24,
        112,
        isError
            ? error
            : text
    );

    drawText(
        app,
        app.fontSmall,
        fitTextToWidth(
            app.fontSmall,
            app.loadingDetail,
            width - 48
        ),
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
        height - 24,
        muted
    );

    presentBookFrame(
        app,
        portraitFrame
    );
}

void renderReader(
    App& app)
{
    const bool portraitFrame =
        beginBookFrame(
            app
        );

    const int width =
        canvasWidth(
            app
        );

    const int height =
        canvasHeight(
            app
        );

    const int textLeft =
        readerTextLeft(
            app
        );

    const int textWidth =
        readerTextWidth(
            app
        );

    const SDL_Color background
    {
        244,
        239,
        225,
        255
    };

    const SDL_Color header
    {
        224,
        218,
        201,
        255
    };

    const SDL_Color text
    {
        36,
        34,
        30,
        255
    };

    const SDL_Color muted
    {
        100,
        95,
        85,
        255
    };

    const SDL_Color error
    {
        150,
        70,
        70,
        255
    };

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
        width,
        36
    };

    SDL_SetRenderDrawColor(
        app.renderer,
        header.r,
        header.g,
        header.b,
        header.a
    );

    fillLogicalRect(
        app,
        headerRect
    );

    std::string logicalIndicator;

    if (
        app.readerActiveTocIndex >= 0 &&
        app.readerActiveTocIndex <
            static_cast<int>(
                app.readerToc.size()
            )
    )
    {
        logicalIndicator =
            std::to_string(
                app.readerActiveTocIndex + 1
            )
            + "/"
            + std::to_string(
                app.readerToc.size()
            );
    }
    else
    {
        logicalIndicator =
            std::to_string(
                app.readerSpineIndex + 1
            )
            + "/"
            + std::to_string(
                app.readerSpine.size()
            );
    }

    const int indicatorWidth =
        textPixelWidth(
            app.fontSmall,
            logicalIndicator
        );

    const int indicatorX =
        width
        - 12
        - indicatorWidth;

    const int titleMaxWidth =
        std::max(
            40,
            indicatorX - 24
        );

    drawText(
        app,
        app.fontSmall,
        fitTextToWidth(
            app.fontSmall,
            currentSectionTitle(
                app
            ),
            titleMaxWidth
        ),
        12,
        10,
        text
    );

    drawText(
        app,
        app.fontSmall,
        logicalIndicator,
        indicatorX,
        10,
        muted
    );

    if (
        app.readerPageIndex >= 0 &&
        app.readerPageIndex <
            static_cast<int>(
                app.readerPages.size()
            )
    )
    {
        const DocumentPage& page =
            app.readerPages[
                app.readerPageIndex
            ];

        if (
            page.kind ==
            PageKind::Text
        )
        {
            int y =
                READER_TOP;

            for (
                const TextLine& line :
                page.lines
            )
            {
                if (
                    !line.text.empty()
                )
                {
                    TTF_Font* lineFont =
                        readerFontForStyle(
                            app,
                            line.style
                        );

                    if (
                        line.centered
                    )
                    {
                        drawCenteredText(
                            app,
                            lineFont,
                            line.text,
                            y,
                            text
                        );
                    }
                    else
                    {
                        drawText(
                            app,
                            lineFont,
                            line.text,
                            textLeft,
                            y,
                            text
                        );
                    }
                }

                y +=
                    READER_LINE_HEIGHT;
            }
        }
        else
        {
            const int imageAreaHeight =
                imageAreaHeightForPage(
                    app,
                    page
                );

            const int imageAreaTop =
                43;

            if (
                app.readerImageTexture
            )
            {
                SDL_Rect destination
                {
                    textLeft
                        + (
                            textWidth
                            - app.readerImageWidth
                        )
                        / 2,

                    imageAreaTop
                        + (
                            imageAreaHeight
                            - app.readerImageHeight
                        )
                        / 2,

                    app.readerImageWidth,
                    app.readerImageHeight
                };

                SDL_Rect physicalDestination =
                    bookPortraitRendering(
                        app
                    )
                    ? rotateLogicalRectClockwise(
                        destination
                    )
                    : destination;

                SDL_RenderCopy(
                    app.renderer,
                    app.readerImageTexture,
                    nullptr,
                    &physicalDestination
                );
            }
            else
            {
                drawCenteredText(
                    app,
                    app.fontSmall,
                    app.readerImageError.empty()
                        ? "Image unavailable"
                        : app.readerImageError,
                    imageAreaTop
                        + imageAreaHeight
                            / 2,
                    error
                );
            }

            int captionY =
                imageAreaTop
                + imageAreaHeight
                + 6;

            for (
                const TextLine& line :
                page.captionLines
            )
            {
                if (
                    !line.text.empty()
                )
                {
                    drawCenteredText(
                        app,
                        smallFontForStyle(
                            app,
                            line.style
                        ),
                        line.text,
                        captionY,
                        text
                    );
                }

                captionY += 16;
            }
        }
    }

    const std::string pageProgress =
        "Pág. "
        + std::to_string(
            app.readerPageIndex + 1
        )
        + "/"
        + std::to_string(
            std::max<std::size_t>(
                1,
                app.readerPages.size()
            )
        );

    if (
        app.orientation ==
        Orientation::Portrait
    )
    {
        const int footerLine1 =
            height - 34;

        const int footerLine2 =
            height - 18;

        drawText(
            app,
            app.fontSmall,
            "L/R Página",
            10,
            footerLine1,
            muted
        );

        const int progressWidth =
            textPixelWidth(
                app.fontSmall,
                pageProgress
            );

        drawText(
            app,
            app.fontSmall,
            pageProgress,
            width
                - 10
                - progressWidth,
            footerLine1,
            muted
        );

        drawReaderFooterControls(
            app,
            10,
            footerLine2,
            muted
        );
    }
    else
    {
        constexpr int footerY =
            253;

        drawText(
            app,
            app.fontSmall,
            "L/R Página",
            14,
            footerY,
            muted
        );

        const int controlsX =
            14
            + textPixelWidth(
                app.fontSmall,
                "L/R Página"
            )
            + 12;

        drawReaderFooterControls(
            app,
            controlsX,
            footerY,
            muted
        );

        const int progressWidth =
            textPixelWidth(
                app.fontSmall,
                pageProgress
            );

        drawText(
            app,
            app.fontSmall,
            pageProgress,
            width
                - 14
                - progressWidth,
            253,
            muted
        );
    }

    presentBookFrame(
        app,
        portraitFrame
    );
}

void renderToc(
    App& app)
{
    const bool portraitFrame =
        beginBookFrame(
            app
        );

    const int width =
        canvasWidth(
            app
        );

    const int height =
        canvasHeight(
            app
        );

    const bool portrait =
        app.orientation ==
        Orientation::Portrait;

    const SDL_Color background
    {
        18,
        19,
        20,
        255
    };

    const SDL_Color header
    {
        31,
        33,
        35,
        255
    };

    const SDL_Color selection
    {
        48,
        50,
        50,
        255
    };

    const SDL_Color text
    {
        235,
        235,
        230,
        255
    };

    const SDL_Color muted
    {
        150,
        150,
        145,
        255
    };

    const SDL_Color current
    {
        190,
        190,
        175,
        255
    };

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
        width,
        portrait
            ? 52
            : 45
    };

    SDL_SetRenderDrawColor(
        app.renderer,
        header.r,
        header.g,
        header.b,
        header.a
    );

    fillLogicalRect(
        app,
        headerRect
    );

    if (
        portrait
    )
    {
        drawText(
            app,
            app.fontNormal,
            "Table of Contents",
            12,
            5,
            text
        );

        drawText(
            app,
            app.fontSmall,
            fitTextToWidth(
                app.fontSmall,
                app.readerTitle,
                width - 24
            ),
            12,
            29,
            muted
        );
    }
    else
    {
        drawText(
            app,
            app.fontNormal,
            "Table of Contents",
            16,
            7,
            text
        );

        drawText(
            app,
            app.fontSmall,
            fitTextToWidth(
                app.fontSmall,
                app.readerTitle,
                250
            ),
            214,
            13,
            muted
        );
    }

    if (
        app.readerToc.empty()
    )
    {
        drawCenteredText(
            app,
            app.fontNormal,
            "No chapters",
            height / 2,
            text
        );
    }
    else
    {
        const int visibleItems =
            tocVisibleItems(
                app
            );

        int firstItem =
            app.tocSelected >=
                visibleItems
            ? app.tocSelected
                - visibleItems
                + 1
            : 0;

        const int maxFirst =
            std::max(
                0,
                static_cast<int>(
                    app.readerToc.size()
                )
                - visibleItems
            );

        firstItem =
            std::clamp(
                firstItem,
                0,
                maxFirst
            );

        const int firstY =
            portrait
            ? 60
            : 53;

        for (
            int row = 0;
            row < visibleItems;
            ++row
        )
        {
            const int index =
                firstItem
                + row;

            if (
                index >=
                static_cast<int>(
                    app.readerToc.size()
                )
            )
            {
                break;
            }

            const int y =
                firstY
                + row
                * TOC_ROW_HEIGHT;

            const bool isSelected =
                index ==
                app.tocSelected;

            const bool isCurrent =
                index ==
                app.readerActiveTocIndex;

            if (
                isSelected
            )
            {
                SDL_Rect selectionRect
                {
                    8,
                    y - 3,
                    width - 16,
                    24
                };

                SDL_SetRenderDrawColor(
                    app.renderer,
                    selection.r,
                    selection.g,
                    selection.b,
                    selection.a
                );

                fillLogicalRect(
                    app,
                    selectionRect
                );
            }

            const std::string number =
                std::to_string(
                    index + 1
                )
                + "/"
                + std::to_string(
                    app.readerToc.size()
                );

            const int numberWidth =
                textPixelWidth(
                    app.fontSmall,
                    number
                );

            const int numberX =
                width
                - 12
                - numberWidth;

            std::string title =
                app.readerToc[
                    index
                ].title;

            if (
                isCurrent
            )
            {
                title =
                    "* "
                    + title;
            }

            title =
                fitTextToWidth(
                    app.fontSmall,
                    title,
                    numberX - 24
                );

            drawText(
                app,
                app.fontSmall,
                title,
                12,
                y,
                isCurrent
                    ? current
                    : text
            );

            drawText(
                app,
                app.fontSmall,
                number,
                numberX,
                y,
                muted
            );
        }
    }

    drawText(
        app,
        app.fontSmall,
        "X Jump  O Back  TRI Close",
        10,
        height - 19,
        muted
    );

    presentBookFrame(
        app,
        portraitFrame
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

        case Screen::Toc:
        {
            renderToc(
                app
            );

            break;
        }
    }
}

void shutdown(
    App& app)
{
    rememberCurrentProgress(
        app
    );

    saveSettings(
        app
    );

    destroyReaderImage(
        app
    );

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
        app.fontHeading
    )
    {
        TTF_CloseFont(
            app.fontHeading
        );
    }

    if (
        app.fontReaderItalic
    )
    {
        TTF_CloseFont(
            app.fontReaderItalic
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
        app.fontSmallItalic
    )
    {
        TTF_CloseFont(
            app.fontSmallItalic
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
    IMG_Quit();
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
