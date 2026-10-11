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
constexpr int TEXT_LEFT = 22;
constexpr int TEXT_WIDTH = 436;
constexpr int READER_TOP = 45;
constexpr int READER_BOTTOM = 242;
constexpr int READER_LINE_HEIGHT = 18;
constexpr int READER_VISIBLE_LINES = 11;
constexpr int TOC_VISIBLE_ITEMS = 7;
constexpr int TOC_ROW_HEIGHT = 27;
constexpr int MAX_CAPTION_LINES = 4;

constexpr const char* BOOKS_PATH = "ms0:/BOOKS";
constexpr const char* PROGRESS_PATH = "ms0:/PSP/GAME/LarpReader/progress.tsv";
constexpr const char* DEBUG_LOG_PATH = "ms0:/PSP/GAME/LarpReader/debug.log";

constexpr zip_uint64_t MAX_XML_SIZE = 4 * 1024 * 1024;
constexpr zip_uint64_t MAX_IMAGE_SIZE = 16 * 1024 * 1024;

enum class Screen { Library, Loading, Reader, Toc };
enum class LoadingStage { None, OpenStructure, LoadDocument, LayoutDocument, Error };
enum class DocumentPosition { Start, End, Restore };
enum class BlockKind { Text, Image };
enum class PageKind { Text, Image };
enum class TextStyle { Regular, Italic, Heading };

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
    BlockKind kind = BlockKind::Text;
    std::string text;
    std::string imagePath;
    bool centered = false;
    TextStyle style = TextStyle::Regular;
};

struct ChapterDocument {
    std::string path;
    std::vector<ContentBlock> blocks;
    std::string error;
    bool valid = false;
    bool skippable = false;
};

struct TextLine {
    std::string text;
    bool centered = false;
    TextStyle style = TextStyle::Regular;
};

struct DocumentPage {
    PageKind kind = PageKind::Text;
    std::vector<TextLine> lines;
    std::string imagePath;
    std::vector<TextLine> captionLines;
};

struct ReadingProgress {
    int spineIndex = 0;
    int pageIndex = 0;
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
    SDL_GameController* controller = nullptr;

    std::vector<Book> books;
    std::unordered_map<std::string, CachedText> textCache;
    std::unordered_map<std::string, ReadingProgress> savedProgress;

    Screen screen = Screen::Library;
    Screen loadingReturnScreen = Screen::Library;
    LoadingStage loadingStage = LoadingStage::None;

    int selected = 0;
    int currentBookIndex = -1;

    std::string readerBookFilename;
    std::string readerTitle;
    std::vector<SpineItem> readerSpine;
    std::vector<TocEntry> readerToc;
    std::vector<DocumentPage> readerPages;

    int readerSpineIndex = -1;
    int readerPageIndex = 0;
    int readerActiveTocIndex = -1;
    int tocSelected = 0;

    SDL_Texture* readerImageTexture = nullptr;
    int readerImageWidth = 0;
    int readerImageHeight = 0;
    std::string readerLoadedImagePath;
    std::string readerImageError;

    int pendingSpineIndex = -1;
    int pendingSearchDirection = 1;
    int pendingRestorePage = 0;
    DocumentPosition pendingPosition = DocumentPosition::Start;
    ChapterDocument pendingDocument;

    std::string loadingMessage;
    std::string loadingDetail;
    int loadingWarmupFrames = 0;

    bool running = true;
};

void debugLog(const char* format, ...) {
    FILE* file = std::fopen(DEBUG_LOG_PATH, "a");
    if (!file) return;

    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);

    std::fputc('\n', file);
    std::fflush(file);
    std::fclose(file);
}

void resetDebugLog() {
    FILE* file = std::fopen(DEBUG_LOG_PATH, "w");
    if (!file) return;
    std::fprintf(file, "Larp Reader 0.0.14 debug log\n");
    std::fprintf(file, "============================\n");
    std::fclose(file);
}

std::string toLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

bool endsWithEpub(const std::string& filename) {
    if (filename.size() < 5) return false;
    return toLower(filename.substr(filename.size() - 5)) == ".epub";
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

std::string percentDecode(const std::string& input) {
    std::string output;
    output.reserve(input.size());

    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()) {
            int high = hexValue(input[i + 1]);
            int low = hexValue(input[i + 2]);
            if (high >= 0 && low >= 0) {
                output += static_cast<char>((high << 4) | low);
                i += 2;
                continue;
            }
        }
        output += input[i];
    }

    return output;
}

const char* localName(const char* name) {
    if (!name) return "";
    const char* colon = std::strchr(name, ':');
    return colon ? colon + 1 : name;
}

const char* attributeByLocalName(tinyxml2::XMLElement* element, const char* wanted) {
    if (!element) return nullptr;

    for (
        const tinyxml2::XMLAttribute* a = element->FirstAttribute();
        a;
        a = a->Next()
    ) {
        if (std::strcmp(localName(a->Name()), wanted) == 0) {
            return a->Value();
        }
    }

    return nullptr;
}

tinyxml2::XMLElement* findElementByLocalName(
    tinyxml2::XMLNode* node,
    const char* wanted
) {
    if (!node) return nullptr;

    for (
        tinyxml2::XMLNode* child = node->FirstChild();
        child;
        child = child->NextSibling()
    ) {
        if (tinyxml2::XMLElement* e = child->ToElement()) {
            if (std::strcmp(localName(e->Name()), wanted) == 0) {
                return e;
            }
        }

        if (
            tinyxml2::XMLElement* nested =
                findElementByLocalName(child, wanted)
        ) {
            return nested;
        }
    }

    return nullptr;
}

tinyxml2::XMLElement* findDirectChildByLocalName(
    tinyxml2::XMLElement* parent,
    const char* wanted
) {
    if (!parent) return nullptr;

    for (
        tinyxml2::XMLElement* child = parent->FirstChildElement();
        child;
        child = child->NextSiblingElement()
    ) {
        if (std::strcmp(localName(child->Name()), wanted) == 0) {
            return child;
        }
    }

    return nullptr;
}

std::string elementText(tinyxml2::XMLElement* element) {
    if (!element || !element->GetText()) return {};
    return element->GetText();
}

void replaceAll(
    std::string& value,
    const std::string& from,
    const std::string& to
) {
    if (from.empty()) return;

    std::size_t pos = 0;

    while ((pos = value.find(from, pos)) != std::string::npos) {
        value.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string sanitizeXhtmlEntities(std::string xml) {
    replaceAll(xml, "&nbsp;", "&#160;");
    replaceAll(xml, "&ndash;", "&#8211;");
    replaceAll(xml, "&mdash;", "&#8212;");
    replaceAll(xml, "&hellip;", "&#8230;");
    replaceAll(xml, "&lsquo;", "&#8216;");
    replaceAll(xml, "&rsquo;", "&#8217;");
    replaceAll(xml, "&ldquo;", "&#8220;");
    replaceAll(xml, "&rdquo;", "&#8221;");
    replaceAll(xml, "&laquo;", "&#171;");
    replaceAll(xml, "&raquo;", "&#187;");
    replaceAll(xml, "&copy;", "&#169;");
    replaceAll(xml, "&reg;", "&#174;");
    replaceAll(xml, "&trade;", "&#8482;");
    return xml;
}

std::string normalizeText(const std::string& input) {
    std::string output;
    bool pendingSpace = false;

    for (std::size_t i = 0; i < input.size(); ++i) {
        unsigned char c =
            static_cast<unsigned char>(input[i]);

        if (
            c == 0xC2 &&
            i + 1 < input.size() &&
            static_cast<unsigned char>(input[i + 1]) == 0xA0
        ) {
            pendingSpace = !output.empty();
            ++i;
            continue;
        }

        if (std::isspace(c)) {
            pendingSpace = !output.empty();
            continue;
        }

        if (pendingSpace && !output.empty()) {
            output += ' ';
        }

        pendingSpace = false;
        output += static_cast<char>(c);
    }

    while (!output.empty() && output.back() == ' ') {
        output.pop_back();
    }

    return output;
}

void appendNodeText(
    tinyxml2::XMLNode* node,
    std::string& output
) {
    if (!node) return;

    if (tinyxml2::XMLText* text = node->ToText()) {
        if (text->Value()) {
            output += text->Value();
        }

        return;
    }

    tinyxml2::XMLElement* element =
        node->ToElement();

    if (element) {
        const std::string name =
            toLower(localName(element->Name()));

        if (
            name == "style" ||
            name == "script" ||
            name == "img" ||
            name == "svg"
        ) {
            return;
        }

        if (name == "br") {
            output += ' ';
        }
    }

    for (
        tinyxml2::XMLNode* child = node->FirstChild();
        child;
        child = child->NextSibling()
    ) {
        appendNodeText(child, output);
    }
}

std::string nodeText(tinyxml2::XMLNode* node) {
    std::string raw;
    appendNodeText(node, raw);
    return normalizeText(raw);
}

bool readZipEntry(
    zip_t* archive,
    const std::string& entryName,
    std::string& output,
    std::string& error
) {
    zip_stat_t statInfo;
    zip_stat_init(&statInfo);

    if (
        zip_stat(
            archive,
            entryName.c_str(),
            0,
            &statInfo
        ) != 0
    ) {
        error =
            "Missing ZIP entry: "
            + entryName;

        return false;
    }

    if (statInfo.size > MAX_XML_SIZE) {
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

    if (!file) {
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

    while (totalRead < statInfo.size) {
        zip_int64_t readNow =
            zip_fread(
                file,
                output.data()
                    + static_cast<std::size_t>(
                        totalRead
                    ),
                statInfo.size - totalRead
            );

        if (readNow < 0) {
            zip_fclose(file);

            error =
                "Error reading ZIP entry: "
                + entryName;

            return false;
        }

        if (readNow == 0) {
            break;
        }

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

bool readZipEntryBytes(
    zip_t* archive,
    const std::string& entryName,
    std::vector<std::uint8_t>& output,
    std::string& error
) {
    zip_stat_t statInfo;
    zip_stat_init(&statInfo);

    if (
        zip_stat(
            archive,
            entryName.c_str(),
            0,
            &statInfo
        ) != 0
    ) {
        error =
            "Missing image: "
            + entryName;

        return false;
    }

    if (statInfo.size > MAX_IMAGE_SIZE) {
        error = "Image too large";
        return false;
    }

    zip_file_t* file =
        zip_fopen(
            archive,
            entryName.c_str(),
            0
        );

    if (!file) {
        error = "Could not open image";
        return false;
    }

    output.resize(
        static_cast<std::size_t>(
            statInfo.size
        )
    );

    zip_uint64_t totalRead = 0;

    while (totalRead < statInfo.size) {
        zip_int64_t readNow =
            zip_fread(
                file,
                output.data()
                    + static_cast<std::size_t>(
                        totalRead
                    ),
                statInfo.size - totalRead
            );

        if (readNow < 0) {
            zip_fclose(file);
            error = "Could not read image";
            return false;
        }

        if (readNow == 0) {
            break;
        }

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
    const std::string& path
) {
    std::size_t pos =
        path.find_last_of('/');

    if (pos == std::string::npos) {
        return {};
    }

    return path.substr(0, pos + 1);
}

std::string normalizeArchivePath(
    const std::string& path
) {
    std::vector<std::string> parts;
    std::size_t start = 0;

    while (start <= path.size()) {
        std::size_t slash =
            path.find('/', start);

        std::size_t end =
            slash == std::string::npos
            ? path.size()
            : slash;

        std::string part =
            path.substr(
                start,
                end - start
            );

        if (
            !part.empty() &&
            part != "."
        ) {
            if (part == "..") {
                if (!parts.empty()) {
                    parts.pop_back();
                }
            } else {
                parts.push_back(part);
            }
        }

        if (slash == std::string::npos) {
            break;
        }

        start = slash + 1;
    }

    std::string result;

    for (
        std::size_t i = 0;
        i < parts.size();
        ++i
    ) {
        if (i) {
            result += '/';
        }

        result += parts[i];
    }

    return result;
}

struct ArchiveReference {
    std::string path;
    std::string fragment;
};

ArchiveReference resolveArchiveReference(
    const std::string& baseDocumentPath,
    const std::string& href
) {
    ArchiveReference result;

    std::string cleanHref =
        href;

    std::size_t fragmentPos =
        cleanHref.find('#');

    if (fragmentPos != std::string::npos) {
        result.fragment =
            percentDecode(
                cleanHref.substr(
                    fragmentPos + 1
                )
            );

        cleanHref.erase(fragmentPos);
    }

    std::size_t queryPos =
        cleanHref.find('?');

    if (queryPos != std::string::npos) {
        cleanHref.erase(queryPos);
    }

    cleanHref =
        percentDecode(cleanHref);

    if (cleanHref.empty()) {
        result.path =
            normalizeArchivePath(
                baseDocumentPath
            );
    } else {
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
    const std::string& href
) {
    return
        resolveArchiveReference(
            baseDocumentPath,
            href
        ).path;
}

bool propertyContainsToken(
    const std::string& properties,
    const std::string& token
) {
    std::size_t start = 0;

    while (start < properties.size()) {
        while (
            start < properties.size() &&
            std::isspace(
                static_cast<unsigned char>(
                    properties[start]
                )
            )
        ) {
            ++start;
        }

        if (start >= properties.size()) {
            break;
        }

        std::size_t end =
            start;

        while (
            end < properties.size() &&
            !std::isspace(
                static_cast<unsigned char>(
                    properties[end]
                )
            )
        ) {
            ++end;
        }

        if (
            properties.substr(
                start,
                end - start
            ) == token
        ) {
            return true;
        }

        start = end;
    }

    return false;
}

bool parseContainer(
    zip_t* archive,
    std::string& opfPath,
    std::string& error
) {
    std::string xml;

    if (
        !readZipEntry(
            archive,
            "META-INF/container.xml",
            xml,
            error
        )
    ) {
        return false;
    }

    tinyxml2::XMLDocument document;

    if (
        document.Parse(
            xml.data(),
            xml.size()
        ) != tinyxml2::XML_SUCCESS
    ) {
        error = "Invalid container.xml";
        return false;
    }

    tinyxml2::XMLElement* rootfile =
        findElementByLocalName(
            &document,
            "rootfile"
        );

    if (!rootfile) {
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
    ) {
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
    Book& book
) {
    std::string xml;
    std::string error;

    if (
        !readZipEntry(
            archive,
            opfPath,
            xml,
            error
        )
    ) {
        book.error = error;
        return false;
    }

    tinyxml2::XMLDocument document;

    if (
        document.Parse(
            xml.data(),
            xml.size()
        ) != tinyxml2::XML_SUCCESS
    ) {
        book.error =
            "Invalid OPF XML";

        return false;
    }

    tinyxml2::XMLElement* metadata =
        findElementByLocalName(
            &document,
            "metadata"
        );

    if (!metadata) {
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

    if (book.title.empty()) {
        book.title =
            book.filename;
    }

    if (book.author.empty()) {
        book.author =
            "Unknown author";
    }

    return true;
}

Book loadBook(
    const std::string& filename
) {
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

    if (!archive) {
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
    ) {
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
    ) {
        zip_close(archive);
        return book;
    }

    zip_close(archive);

    book.valid = true;

    return book;
}

std::vector<Book> scanBooks() {
    std::vector<std::string>
        filenames;

    DIR* directory =
        opendir(
            BOOKS_PATH
        );

    if (!directory) {
        return {};
    }

    while (
        dirent* entry =
            readdir(directory)
    ) {
        std::string filename =
            entry->d_name;

        if (
            filename == "." ||
            filename == ".."
        ) {
            continue;
        }

        if (endsWithEpub(filename)) {
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
    ) {
        books.push_back(
            loadBook(filename)
        );
    }

    return books;
}

void appendNavEntry(
    std::vector<TocEntry>& entries,
    const std::string& title,
    const ArchiveReference& ref
) {
    if (
        title.empty() ||
        ref.path.empty()
    ) {
        return;
    }

    for (
        const TocEntry& existing :
        entries
    ) {
        if (
            existing.path == ref.path &&
            existing.fragment == ref.fragment &&
            existing.title == title
        ) {
            return;
        }
    }

    TocEntry entry;

    entry.title = title;
    entry.path = ref.path;
    entry.fragment = ref.fragment;

    entries.push_back(
        std::move(entry)
    );
}

tinyxml2::XMLElement* findNavToc(
    tinyxml2::XMLNode* node
) {
    if (!node) return nullptr;

    for (
        tinyxml2::XMLNode* child =
            node->FirstChild();

        child;

        child =
            child->NextSibling()
    ) {
        if (
            tinyxml2::XMLElement* element =
                child->ToElement()
        ) {
            if (
                std::strcmp(
                    localName(
                        element->Name()
                    ),
                    "nav"
                ) == 0
            ) {
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
                ) {
                    return element;
                }
            }
        }

        if (
            tinyxml2::XMLElement* nested =
                findNavToc(child)
        ) {
            return nested;
        }
    }

    return nullptr;
}

void collectNavLinks(
    tinyxml2::XMLNode* node,
    const std::string& navPath,
    std::vector<TocEntry>& entries
) {
    if (!node) return;

    if (
        tinyxml2::XMLElement* element =
            node->ToElement()
    ) {
        if (
            std::strcmp(
                localName(
                    element->Name()
                ),
                "a"
            ) == 0
        ) {
            const char* href =
                attributeByLocalName(
                    element,
                    "href"
                );

            if (
                href &&
                href[0] != '\0'
            ) {
                appendNavEntry(
                    entries,
                    nodeText(element),
                    resolveArchiveReference(
                        navPath,
                        href
                    )
                );
            }
        }
    }

    for (
        tinyxml2::XMLNode* child =
            node->FirstChild();

        child;

        child =
            child->NextSibling()
    ) {
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
    std::vector<TocEntry>& entries
) {
    if (!node) return;

    for (
        tinyxml2::XMLNode* child =
            node->FirstChild();

        child;

        child =
            child->NextSibling()
    ) {
        tinyxml2::XMLElement* element =
            child->ToElement();

        if (!element) continue;

        if (
            std::strcmp(
                localName(
                    element->Name()
                ),
                "navPoint"
            ) == 0
        ) {
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
                ? content->Attribute("src")
                : nullptr;

            if (
                src &&
                src[0] != '\0'
            ) {
                std::string title =
                    textElement
                    ? nodeText(textElement)
                    : nodeText(navLabel);

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

std::vector<TocEntry> loadNavigationEntries(
    zip_t* archive,
    const std::string& opfPath,
    const std::unordered_map<
        std::string,
        ManifestItem
    >& manifestItems,
    tinyxml2::XMLElement* spine
) {
    std::vector<TocEntry>
        navEntries;

    for (
        const auto& pair :
        manifestItems
    ) {
        const ManifestItem& item =
            pair.second;

        if (
            item.mediaType !=
                "application/xhtml+xml" ||
            !propertyContainsToken(
                item.properties,
                "nav"
            )
        ) {
            continue;
        }

        std::string navPath =
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
        ) {
            continue;
        }

        xml =
            sanitizeXhtmlEntities(
                std::move(xml)
            );

        tinyxml2::XMLDocument document;

        if (
            document.Parse(
                xml.data(),
                xml.size()
            ) != tinyxml2::XML_SUCCESS
        ) {
            continue;
        }

        tinyxml2::XMLElement* nav =
            findNavToc(
                &document
            );

        if (!nav) {
            nav =
                findElementByLocalName(
                    &document,
                    "nav"
                );
        }

        if (nav) {
            collectNavLinks(
                nav,
                navPath,
                navEntries
            );
        }
    }

    if (!navEntries.empty()) {
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
        ? spine->Attribute("toc")
        : nullptr;

    if (tocId) {
        auto found =
            manifestItems.find(
                tocId
            );

        if (
            found !=
            manifestItems.end()
        ) {
            ncxItem =
                &found->second;
        }
    }

    if (!ncxItem) {
        for (
            const auto& pair :
            manifestItems
        ) {
            if (
                pair.second.mediaType ==
                "application/x-dtbncx+xml"
            ) {
                ncxItem =
                    &pair.second;

                break;
            }
        }
    }

    if (!ncxItem) {
        return {};
    }

    std::string ncxPath =
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
    ) {
        return {};
    }

    tinyxml2::XMLDocument document;

    if (
        document.Parse(
            xml.data(),
            xml.size()
        ) != tinyxml2::XML_SUCCESS
    ) {
        return {};
    }

    tinyxml2::XMLElement* navMap =
        findElementByLocalName(
            &document,
            "navMap"
        );

    std::vector<TocEntry>
        ncxEntries;

    if (navMap) {
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
    int index
) {
    std::string filename =
        path;

    std::size_t slash =
        filename.find_last_of('/');

    if (
        slash != std::string::npos
    ) {
        filename.erase(
            0,
            slash + 1
        );
    }

    std::string lower =
        toLower(filename);

    if (
        lower.find("cover") != std::string::npos ||
        lower.find("cubierta") != std::string::npos
    ) {
        return "Cover";
    }

    if (
        lower.find("title") != std::string::npos
    ) {
        return "Title page";
    }

    if (
        lower.find("prologo") != std::string::npos
    ) {
        return "Prólogo";
    }

    if (
        lower.find("epilogo") != std::string::npos
    ) {
        return "Epílogo";
    }

    if (
        lower.find("autor") != std::string::npos
    ) {
        return "Autor";
    }

    if (
        lower.find("editor") != std::string::npos
    ) {
        return "Editor";
    }

    if (
        lower.find("info") != std::string::npos
    ) {
        return "Información";
    }

    if (
        lower.find("sinopsis") != std::string::npos
    ) {
        return "Sinopsis";
    }

    if (
        lower.find("about") != std::string::npos
    ) {
        return "About";
    }

    if (
        lower.find("copyright") != std::string::npos
    ) {
        return "Copyright";
    }

    return
        "Section "
        + std::to_string(index + 1);
}

int findSpineIndexByPath(
    const std::vector<SpineItem>& spine,
    const std::string& path
) {
    for (
        std::size_t i = 0;
        i < spine.size();
        ++i
    ) {
        if (
            spine[i].path ==
            path
        ) {
            return
                static_cast<int>(i);
        }
    }

    return -1;
}

bool tocHasSpineIndex(
    const std::vector<TocEntry>& toc,
    int spineIndex
) {
    for (
        const TocEntry& entry :
        toc
    ) {
        if (
            entry.spineIndex ==
            spineIndex
        ) {
            return true;
        }
    }

    return false;
}

ReaderStructure loadReaderStructure(
    const Book& book
) {
    ReaderStructure result;

    int zipError = 0;

    zip_t* archive =
        zip_open(
            book.path.c_str(),
            ZIP_RDONLY,
            &zipError
        );

    if (!archive) {
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
    ) {
        result.error = error;
        zip_close(archive);
        return result;
    }

    tinyxml2::XMLDocument document;

    if (
        document.Parse(
            opfXml.data(),
            opfXml.size()
        ) != tinyxml2::XML_SUCCESS
    ) {
        result.error =
            "Invalid OPF";

        zip_close(archive);
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
    ) {
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

        item =
            item->NextSiblingElement()
    ) {
        if (
            std::strcmp(
                localName(
                    item->Name()
                ),
                "item"
            ) != 0
        ) {
            continue;
        }

        const char* id =
            item->Attribute("id");

        const char* href =
            item->Attribute("href");

        if (
            !id ||
            !href
        ) {
            continue;
        }

        ManifestItem m;

        m.id = id;
        m.href = href;

        if (
            const char* media =
                item->Attribute(
                    "media-type"
                )
        ) {
            m.mediaType = media;
        }

        if (
            const char* props =
                item->Attribute(
                    "properties"
                )
        ) {
            m.properties = props;
        }

        manifestItems[
            m.id
        ] = std::move(m);
    }

    for (
        tinyxml2::XMLElement* itemRef =
            spine->FirstChildElement();

        itemRef;

        itemRef =
            itemRef->NextSiblingElement()
    ) {
        if (
            std::strcmp(
                localName(
                    itemRef->Name()
                ),
                "itemref"
            ) != 0
        ) {
            continue;
        }

        const char* idref =
            itemRef->Attribute(
                "idref"
            );

        if (!idref) {
            continue;
        }

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
        ) {
            continue;
        }

        auto found =
            manifestItems.find(
                idref
            );

        if (
            found ==
            manifestItems.end()
        ) {
            continue;
        }

        const ManifestItem& item =
            found->second;

        if (
            item.mediaType !=
            "application/xhtml+xml"
        ) {
            continue;
        }

        if (
            propertyContainsToken(
                item.properties,
                "nav"
            )
        ) {
            continue;
        }

        SpineItem s;

        s.path =
            resolveArchivePath(
                book.opfPath,
                item.href
            );

        if (
            s.path.empty()
        ) {
            continue;
        }

        s.title =
            fallbackSpineTitle(
                s.path,
                static_cast<int>(
                    result.spine.size()
                )
            );

        result.spine.push_back(
            std::move(s)
        );
    }

    if (
        result.spine.empty()
    ) {
        result.error =
            "No readable spine items";

        zip_close(archive);
        return result;
    }

    std::vector<TocEntry> rawToc =
        loadNavigationEntries(
            archive,
            book.opfPath,
            manifestItems,
            spine
        );

    zip_close(archive);

    for (
        TocEntry entry :
        rawToc
    ) {
        entry.spineIndex =
            findSpineIndexByPath(
                result.spine,
                entry.path
            );

        if (
            entry.spineIndex < 0
        ) {
            continue;
        }

        result.toc.push_back(
            std::move(entry)
        );
    }

    if (
        !result.spine.empty() &&
        !tocHasSpineIndex(
            result.toc,
            0
        )
    ) {
        std::string firstLower =
            toLower(
                result.spine[0].path
            );

        if (
            firstLower.find("cover") != std::string::npos ||
            firstLower.find("cubierta") != std::string::npos
        ) {
            TocEntry cover;

            cover.title =
                "Cover";

            cover.path =
                result.spine[0].path;

            cover.spineIndex = 0;

            result.toc.insert(
                result.toc.begin(),
                std::move(cover)
            );
        }
    }

    if (
        result.toc.empty()
    ) {
        for (
            std::size_t i = 0;
            i < result.spine.size();
            ++i
        ) {
            TocEntry entry;

            entry.title =
                result.spine[i].title;

            entry.path =
                result.spine[i].path;

            entry.spineIndex =
                static_cast<int>(i);

            result.toc.push_back(
                std::move(entry)
            );
        }
    }

    for (
        const TocEntry& entry :
        result.toc
    ) {
        if (
            entry.fragment.empty()
        ) {
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
    ) {
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
    ) {
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

    result.valid = true;

    return result;
}

bool isIgnoredElementName(
    const std::string& name
) {
    return
        name == "head" ||
        name == "style" ||
        name == "script" ||
        name == "noscript";
}

bool isTextBlockName(
    const std::string& name
) {
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

bool classContains(
    const char* classValue,
    const char* token
) {
    if (
        !classValue ||
        !token
    ) {
        return false;
    }

    std::string classes =
        toLower(classValue);

    return
        classes.find(
            toLower(token)
        ) != std::string::npos;
}

bool styleContainsCentered(
    const char* styleValue
) {
    if (!styleValue) {
        return false;
    }

    std::string style =
        toLower(styleValue);

    return
        style.find("text-align") != std::string::npos &&
        style.find("center") != std::string::npos;
}

bool elementIsCentered(
    tinyxml2::XMLElement* element
) {
    if (!element) {
        return false;
    }

    std::string name =
        toLower(
            localName(
                element->Name()
            )
        );

    if (
        name == "h1" ||
        name == "h2" ||
        name == "h3" ||
        name == "h4" ||
        name == "h5" ||
        name == "h6"
    ) {
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
    ) {
        return true;
    }

    return
        styleContainsCentered(
            element->Attribute(
                "style"
            )
        );
}

bool isHeadingName(
    const std::string& name
) {
    return
        name == "h1" ||
        name == "h2" ||
        name == "h3" ||
        name == "h4" ||
        name == "h5" ||
        name == "h6";
}

bool hasNonWhitespaceDirectText(
    tinyxml2::XMLElement* element
) {
    if (!element) {
        return false;
    }

    for (
        tinyxml2::XMLNode* child =
            element->FirstChild();

        child;

        child =
            child->NextSibling()
    ) {
        tinyxml2::XMLText* text =
            child->ToText();

        if (
            !text ||
            !text->Value()
        ) {
            continue;
        }

        const char* value =
            text->Value();

        for (
            const unsigned char* p =
                reinterpret_cast<
                    const unsigned char*
                >(value);

            *p;

            ++p
        ) {
            if (!std::isspace(*p)) {
                return true;
            }
        }
    }

    return false;
}

bool blockIsEntirelyItalic(
    tinyxml2::XMLElement* element
) {
    if (!element) {
        return false;
    }

    const char* styleValue =
        element->Attribute(
            "style"
        );

    if (styleValue) {
        std::string style =
            toLower(styleValue);

        if (
            style.find("font-style") != std::string::npos &&
            style.find("italic") != std::string::npos
        ) {
            return true;
        }
    }

    const char* classValue =
        element->Attribute(
            "class"
        );

    if (classValue) {
        std::string classes =
            toLower(classValue);

        if (
            classes.find("italic") != std::string::npos ||
            classes.find("cursiva") != std::string::npos
        ) {
            return true;
        }
    }

    if (
        hasNonWhitespaceDirectText(
            element
        )
    ) {
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
    ) {
        std::string childName =
            toLower(
                localName(
                    child->Name()
                )
            );

        if (
            childName == "i" ||
            childName == "em"
        ) {
            foundStyledChild =
                true;

            continue;
        }

        if (
            childName == "br"
        ) {
            continue;
        }

        return false;
    }

    return foundStyledChild;
}

TextStyle textStyleForElement(
    tinyxml2::XMLElement* element,
    const std::string& name
) {
    if (
        isHeadingName(name)
    ) {
        return
            TextStyle::Heading;
    }

    if (
        blockIsEntirelyItalic(
            element
        )
    ) {
        return
            TextStyle::Italic;
    }

    return
        TextStyle::Regular;
}

bool hasDirectStructuralChild(
    tinyxml2::XMLElement* element
) {
    if (!element) {
        return false;
    }

    for (
        tinyxml2::XMLElement* child =
            element->FirstChildElement();

        child;

        child =
            child->NextSiblingElement()
    ) {
        std::string name =
            toLower(
                localName(
                    child->Name()
                )
            );

        if (
            name == "img" ||
            name == "svg" ||
            isTextBlockName(name)
        ) {
            return true;
        }
    }

    return false;
}

std::string firstSvgImageReference(
    tinyxml2::XMLNode* node
) {
    if (!node) {
        return {};
    }

    if (
        tinyxml2::XMLElement* element =
            node->ToElement()
    ) {
        std::string name =
            toLower(
                localName(
                    element->Name()
                )
            );

        if (
            name == "image"
        ) {
            const char* href =
                attributeByLocalName(
                    element,
                    "href"
                );

            if (
                href &&
                href[0] != '\0'
            ) {
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
    ) {
        std::string result =
            firstSvgImageReference(
                child
            );

        if (!result.empty()) {
            return result;
        }
    }

    return {};
}

void collectContentBlocks(
    tinyxml2::XMLNode* node,
    const std::string& xhtmlPath,
    bool inheritedCentered,
    std::vector<ContentBlock>& blocks
) {
    if (!node) {
        return;
    }

    tinyxml2::XMLElement* element =
        node->ToElement();

    if (!element) {
        for (
            tinyxml2::XMLNode* child =
                node->FirstChild();

            child;

            child =
                child->NextSibling()
        ) {
            collectContentBlocks(
                child,
                xhtmlPath,
                inheritedCentered,
                blocks
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
        isIgnoredElementName(name)
    ) {
        return;
    }

    bool centered =
        inheritedCentered ||
        elementIsCentered(element);

    if (
        name == "img"
    ) {
        const char* src =
            attributeByLocalName(
                element,
                "src"
            );

        if (
            src &&
            src[0] != '\0'
        ) {
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
                std::move(block)
            );
        }

        return;
    }

    if (
        name == "svg"
    ) {
        std::string href =
            firstSvgImageReference(
                element
            );

        if (
            !href.empty()
        ) {
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
                std::move(block)
            );
        }

        return;
    }

    if (
        isTextBlockName(name) &&
        !hasDirectStructuralChild(
            element
        )
    ) {
        std::string text =
            nodeText(element);

        if (
            !text.empty()
        ) {
            ContentBlock block;

            block.kind =
                BlockKind::Text;

            block.text =
                std::move(text);

            block.centered =
                centered;

            block.style =
                textStyleForElement(
                    element,
                    name
                );

            blocks.push_back(
                std::move(block)
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
    ) {
        collectContentBlocks(
            child,
            xhtmlPath,
            centered,
            blocks
        );
    }
}

ChapterDocument loadChapterDocument(
    const Book& book,
    const std::string& chapterPath
) {
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

    if (!archive) {
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
    ) {
        zip_close(archive);
        return result;
    }

    zip_close(archive);

    xml =
        sanitizeXhtmlEntities(
            std::move(xml)
        );

    tinyxml2::XMLDocument document;

    tinyxml2::XMLError parseResult =
        document.Parse(
            xml.data(),
            xml.size()
        );

    if (
        parseResult !=
        tinyxml2::XML_SUCCESS
    ) {
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

    if (!body) {
        result.error =
            "XHTML has no body";

        result.skippable =
            true;

        return result;
    }

    collectContentBlocks(
        body,
        chapterPath,
        elementIsCentered(body),
        result.blocks
    );

    if (
        result.blocks.empty()
    ) {
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
    ) {
        if (
            block.kind ==
            BlockKind::Image
        ) {
            ++imageBlocks;
        } else {
            ++textBlocks;
        }
    }

    debugLog(
        "DOCUMENT: %s blocks=%zu text=%d images=%d",
        chapterPath.c_str(),
        result.blocks.size(),
        textBlocks,
        imageBlocks
    );

    result.valid = true;

    return result;
}

TTF_Font* loadFont(int size) {
    const char* paths[] =
    {
        "assets/fonts/NotoSans-Regular.ttf",

        "ms0:/PSP/GAME/LarpReader/"
        "assets/fonts/NotoSans-Regular.ttf"
    };

    for (
        const char* path :
        paths
    ) {
        if (
            TTF_Font* font =
                TTF_OpenFont(
                    path,
                    size
                )
        ) {
            return font;
        }
    }

    return nullptr;
}

int textPixelWidth(
    TTF_Font* font,
    const std::string& text
) {
    if (
        !font ||
        text.empty()
    ) {
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
    ) {
        return 0;
    }

    return width;
}

std::string fitTextToWidth(
    TTF_Font* font,
    const std::string& text,
    int maxWidth
) {
    if (
        textPixelWidth(
            font,
            text
        ) <= maxWidth
    ) {
        return text;
    }

    const std::string ellipsis =
        "...";

    std::string result =
        text;

    while (!result.empty()) {
        std::size_t start =
            result.size() - 1;

        while (
            start > 0 &&
            (
                static_cast<unsigned char>(
                    result[start]
                ) &
                0xC0
            ) == 0x80
        ) {
            --start;
        }

        result.erase(start);

        if (
            textPixelWidth(
                font,
                result + ellipsis
            ) <= maxWidth
        ) {
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
    >& widthCache
) {
    auto found =
        widthCache.find(text);

    if (
        found !=
        widthCache.end()
    ) {
        return found->second;
    }

    int width =
        textPixelWidth(
            font,
            text
        );

    if (
        widthCache.size() <
        4096
    ) {
        widthCache[text] =
            width;
    }

    return width;
}

std::vector<std::string> wrapParagraphFast(
    TTF_Font* font,
    const std::string& paragraph,
    int maxWidth,
    int spaceWidth,
    std::unordered_map<
        std::string,
        int
    >& widthCache
) {
    std::vector<std::string>
        lines;

    std::string currentLine;
    int currentWidth = 0;
    std::size_t start = 0;

    while (
        start <
        paragraph.size()
    ) {
        while (
            start <
                paragraph.size() &&
            paragraph[start] == ' '
        ) {
            ++start;
        }

        if (
            start >=
            paragraph.size()
        ) {
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
        ) {
            end =
                paragraph.size();
        }

        std::string word =
            paragraph.substr(
                start,
                end - start
            );

        int wordWidth =
            measureTextWidth(
                font,
                word,
                widthCache
            );

        if (
            currentLine.empty()
        ) {
            currentLine =
                word;

            currentWidth =
                wordWidth;
        } else {
            int candidateWidth =
                currentWidth
                + spaceWidth
                + wordWidth;

            if (
                candidateWidth <=
                maxWidth
            ) {
                currentLine += " ";
                currentLine += word;
                currentWidth =
                    candidateWidth;
            } else {
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
    ) {
        lines.push_back(
            currentLine
        );
    }

    return lines;
}

TTF_Font* readerFontForStyle(
    App& app,
    TextStyle style
) {
    switch (style) {
        case TextStyle::Italic:
            return
                app.fontReaderItalic
                ? app.fontReaderItalic
                : app.fontReader;

        case TextStyle::Heading:
            return
                app.fontHeading
                ? app.fontHeading
                : app.fontReader;

        case TextStyle::Regular:
        default:
            return
                app.fontReader;
    }
}

TTF_Font* smallFontForStyle(
    App& app,
    TextStyle style
) {
    if (
        style ==
            TextStyle::Italic &&
        app.fontSmallItalic
    ) {
        return
            app.fontSmallItalic;
    }

    return
        app.fontSmall;
}

std::vector<TextLine> wrapTextBlock(
    App& app,
    const ContentBlock& block,
    int maxWidth,
    bool smallText = false
) {
    std::vector<TextLine>
        output;

    std::unordered_map<
        std::string,
        int
    > widthCache;

    widthCache.reserve(256);

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

    std::vector<std::string> lines =
        wrapParagraphFast(
            font,
            block.text,
            maxWidth,
            spaceWidth,
            widthCache
        );

    output.reserve(
        lines.size()
    );

    for (
        std::string& line :
        lines
    ) {
        TextLine textLine;

        textLine.text =
            std::move(line);

        textLine.centered =
            block.centered;

        textLine.style =
            block.style;

        output.push_back(
            std::move(textLine)
        );
    }

    return output;
}

void flushTextPage(
    std::vector<DocumentPage>& pages,
    std::vector<TextLine>& currentLines
) {
    if (
        currentLines.empty()
    ) {
        return;
    }

    DocumentPage page;

    page.kind =
        PageKind::Text;

    page.lines =
        std::move(
            currentLines
        );

    pages.push_back(
        std::move(page)
    );

    currentLines.clear();
}

std::vector<DocumentPage> layoutDocument(
    App& app,
    const ChapterDocument& document
) {
    std::vector<DocumentPage>
        pages;

    std::vector<TextLine>
        currentLines;

    auto appendTextLines =
        [&](
            const std::vector<TextLine>& lines,
            bool paragraphGap
        ) {
            if (
                paragraphGap &&
                !currentLines.empty() &&
                static_cast<int>(
                    currentLines.size()
                ) <
                READER_VISIBLE_LINES
            ) {
                currentLines.push_back(
                    TextLine{
                        "",
                        false,
                        TextStyle::Regular
                    }
                );
            }

            for (
                const TextLine& line :
                lines
            ) {
                if (
                    static_cast<int>(
                        currentLines.size()
                    ) >=
                    READER_VISIBLE_LINES
                ) {
                    flushTextPage(
                        pages,
                        currentLines
                    );
                }

                currentLines.push_back(
                    line
                );
            }
        };

    for (
        std::size_t i = 0;
        i < document.blocks.size();
        ++i
    ) {
        const ContentBlock& block =
            document.blocks[i];

        if (
            block.kind ==
            BlockKind::Text
        ) {
            std::vector<TextLine> wrapped =
                wrapTextBlock(
                    app,
                    block,
                    TEXT_WIDTH
                );

            appendTextLines(
                wrapped,
                true
            );

            continue;
        }

        flushTextPage(
            pages,
            currentLines
        );

        DocumentPage imagePage;

        imagePage.kind =
            PageKind::Image;

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
        ) {
            const ContentBlock& next =
                document.blocks[j];

            if (
                next.kind !=
                    BlockKind::Text ||
                !next.centered
            ) {
                break;
            }

            std::vector<TextLine> wrapped =
                wrapTextBlock(
                    app,
                    next,
                    TEXT_WIDTH - 24,
                    true
                );

            if (
                candidateCaption.size()
                    + wrapped.size() >
                MAX_CAPTION_LINES
            ) {
                break;
            }

            if (
                !candidateCaption.empty()
            ) {
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
                    + wrapped.size() >
                MAX_CAPTION_LINES
            ) {
                if (
                    !candidateCaption.empty() &&
                    candidateCaption.back()
                        .text.empty()
                ) {
                    candidateCaption.pop_back();
                }

                break;
            }

            candidateCaption.insert(
                candidateCaption.end(),
                wrapped.begin(),
                wrapped.end()
            );

            consumedUntil =
                j;
        }

        imagePage.captionLines =
            std::move(
                candidateCaption
            );

        pages.push_back(
            std::move(imagePage)
        );

        i =
            consumedUntil;
    }

    flushTextPage(
        pages,
        currentLines
    );

    return pages;
}

void destroyReaderImage(
    App& app
) {
    if (
        app.readerImageTexture
    ) {
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
    std::string& error
) {
    outputTexture = nullptr;
    outputWidth = 0;
    outputHeight = 0;

    int zipError = 0;

    zip_t* archive =
        zip_open(
            book.path.c_str(),
            ZIP_RDONLY,
            &zipError
        );

    if (!archive) {
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
    ) {
        zip_close(archive);
        return false;
    }

    zip_close(archive);

    if (
        imageBytes.empty()
    ) {
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

    if (!rw) {
        error =
            SDL_GetError();

        return false;
    }

    SDL_Surface* source =
        IMG_Load_RW(
            rw,
            1
        );

    if (!source) {
        error =
            IMG_GetError();

        return false;
    }

    if (
        source->w <= 0 ||
        source->h <= 0
    ) {
        SDL_FreeSurface(source);

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

    double scaleX =
        static_cast<double>(
            maxWidth
        ) /
        static_cast<double>(
            source->w
        );

    double scaleY =
        static_cast<double>(
            maxHeight
        ) /
        static_cast<double>(
            source->h
        );

    double scale =
        std::min(
            1.0,
            std::min(
                scaleX,
                scaleY
            )
        );

    int scaledWidth =
        std::max(
            1,
            static_cast<int>(
                source->w
                * scale
            )
        );

    int scaledHeight =
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
    ) {
        converted =
            SDL_ConvertSurfaceFormat(
                source,
                SDL_PIXELFORMAT_RGBA32,
                0
            );

        if (converted) {
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
            ) {
                finalSurface =
                    scaled;
            } else {
                if (scaled) {
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
        } else {
            debugLog(
                "Image conversion failed, falling back to source texture: %s",
                SDL_GetError()
            );
        }
    }

    SDL_Texture* texture =
        SDL_CreateTextureFromSurface(
            app.renderer,
            finalSurface
        );

    if (!texture) {
        error =
            SDL_GetError();

        if (scaled) {
            SDL_FreeSurface(scaled);
        }

        if (converted) {
            SDL_FreeSurface(converted);
        }

        SDL_FreeSurface(source);

        return false;
    }

    outputTexture =
        texture;

    outputWidth =
        finalSurface->w;

    outputHeight =
        finalSurface->h;

    if (scaled) {
        SDL_FreeSurface(scaled);
    }

    if (converted) {
        SDL_FreeSurface(converted);
    }

    SDL_FreeSurface(source);

    debugLog(
        "IMG texture ready: %s = %dx%d",
        imagePath.c_str(),
        outputWidth,
        outputHeight
    );

    return true;
}

int imageAreaHeightForPage(
    const DocumentPage& page
) {
    int captionHeight =
        static_cast<int>(
            page.captionLines.size()
        )
        * 16;

    if (
        !page.captionLines.empty()
    ) {
        captionHeight += 8;
    }

    return
        std::max(
            80,
            (
                READER_BOTTOM
                - 43
            )
            - captionHeight
        );
}

void loadCurrentPageImage(
    App& app
) {
    destroyReaderImage(app);

    if (
        app.readerPageIndex < 0 ||
        app.readerPageIndex >=
            static_cast<int>(
                app.readerPages.size()
            )
    ) {
        return;
    }

    const DocumentPage& page =
        app.readerPages[
            app.readerPageIndex
        ];

    if (
        page.kind !=
        PageKind::Image
    ) {
        return;
    }

    if (
        app.currentBookIndex < 0 ||
        app.currentBookIndex >=
            static_cast<int>(
                app.books.size()
            )
    ) {
        app.readerImageError =
            "Invalid book";

        return;
    }

    int maxHeight =
        imageAreaHeightForPage(
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
            TEXT_WIDTH,
            maxHeight,
            texture,
            width,
            height,
            error
        )
    ) {
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

void loadProgress(
    App& app
) {
    FILE* file =
        std::fopen(
            PROGRESS_PATH,
            "r"
        );

    if (!file) {
        return;
    }

    bool version2 = false;
    bool firstMeaningfulLine = true;

    char buffer[1024];

    while (
        std::fgets(
            buffer,
            sizeof(buffer),
            file
        )
    ) {
        std::string line =
            buffer;

        while (
            !line.empty() &&
            (
                line.back() == '\n' ||
                line.back() == '\r'
            )
        ) {
            line.pop_back();
        }

        if (
            line.empty()
        ) {
            continue;
        }

        if (
            firstMeaningfulLine &&
            line ==
            "#LARP_PROGRESS_V2"
        ) {
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
        ) {
            continue;
        }

        std::vector<std::string>
            fields;

        std::size_t start = 0;

        while (
            start <=
            line.size()
        ) {
            std::size_t tab =
                line.find(
                    '\t',
                    start
                );

            if (
                tab ==
                std::string::npos
            ) {
                fields.push_back(
                    line.substr(start)
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
        ) {
            continue;
        }

        ReadingProgress progress;

        if (
            fields.size() == 2
        ) {
            progress.spineIndex =
                0;

            int oldTopLine =
                std::max(
                    0,
                    std::atoi(
                        fields[1].c_str()
                    )
                );

            progress.pageIndex =
                oldTopLine /
                READER_VISIBLE_LINES;
        } else {
            progress.spineIndex =
                std::max(
                    0,
                    std::atoi(
                        fields[1].c_str()
                    )
                );

            int rawPosition =
                std::max(
                    0,
                    std::atoi(
                        fields[2].c_str()
                    )
                );

            progress.pageIndex =
                version2
                ? rawPosition
                : rawPosition /
                    READER_VISIBLE_LINES;
        }

        app.savedProgress[
            fields[0]
        ] = progress;
    }

    std::fclose(file);
}

void saveProgress(
    const App& app
) {
    FILE* file =
        std::fopen(
            PROGRESS_PATH,
            "w"
        );

    if (!file) {
        return;
    }

    std::fprintf(
        file,
        "#LARP_PROGRESS_V2\n"
    );

    for (
        const auto& entry :
        app.savedProgress
    ) {
        std::fprintf(
            file,
            "%s\t%d\t%d\n",
            entry.first.c_str(),
            entry.second.spineIndex,
            entry.second.pageIndex
        );
    }

    std::fclose(file);
}

void rememberCurrentProgress(
    App& app
) {
    if (
        app.readerBookFilename.empty() ||
        app.readerSpineIndex < 0
    ) {
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

    app.savedProgress[
        app.readerBookFilename
    ] = progress;

    saveProgress(app);
}

std::string makeTextCacheKey(
    TTF_Font* font,
    const std::string& text,
    SDL_Color color
) {
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
    SDL_Color color
) {
    std::string key =
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
    ) {
        return
            &existing->second;
    }

    SDL_Surface* surface =
        TTF_RenderUTF8_Blended(
            font,
            text.c_str(),
            color
        );

    if (!surface) {
        return nullptr;
    }

    SDL_Texture* texture =
        SDL_CreateTextureFromSurface(
            app.renderer,
            surface
        );

    if (!texture) {
        SDL_FreeSurface(surface);
        return nullptr;
    }

    CachedText cached;

    cached.texture =
        texture;

    cached.width =
        surface->w;

    cached.height =
        surface->h;

    SDL_FreeSurface(surface);

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
    SDL_Color color
) {
    if (
        text.empty()
    ) {
        return;
    }

    CachedText* cached =
        getCachedText(
            app,
            font,
            text,
            color
        );

    if (!cached) {
        return;
    }

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

void drawCenteredText(
    App& app,
    TTF_Font* font,
    const std::string& text,
    int y,
    SDL_Color color
) {
    if (
        text.empty()
    ) {
        return;
    }

    int width =
        textPixelWidth(
            font,
            text
        );

    drawText(
        app,
        font,
        text,
        std::max(
            TEXT_LEFT,
            (
                SCREEN_WIDTH
                - width
            ) / 2
        ),
        y,
        color
    );
}

void clearTextCache(
    App& app
) {
    for (
        auto& entry :
        app.textCache
    ) {
        if (
            entry.second.texture
        ) {
            SDL_DestroyTexture(
                entry.second.texture
            );
        }
    }

    app.textCache.clear();
}

int activeTocIndexForSpine(
    const App& app,
    int spineIndex
) {
    int active = -1;

    for (
        std::size_t i = 0;
        i < app.readerToc.size();
        ++i
    ) {
        if (
            app.readerToc[i].spineIndex <=
            spineIndex
        ) {
            active =
                static_cast<int>(i);
        } else {
            break;
        }
    }

    return active;
}

std::string currentSectionTitle(
    const App& app
) {
    if (
        app.readerActiveTocIndex >= 0 &&
        app.readerActiveTocIndex <
            static_cast<int>(
                app.readerToc.size()
            )
    ) {
        const TocEntry& entry =
            app.readerToc[
                app.readerActiveTocIndex
            ];

        std::string lowerTitle =
            toLower(entry.title);

        bool coverLike =
            lowerTitle.find("cover") != std::string::npos ||
            lowerTitle.find("cubierta") != std::string::npos;

        if (
            !(
                coverLike &&
                app.readerSpineIndex >
                    entry.spineIndex
            )
        ) {
            return entry.title;
        }
    }

    if (
        app.readerSpineIndex >= 0 &&
        app.readerSpineIndex <
            static_cast<int>(
                app.readerSpine.size()
            )
    ) {
        return
            app.readerSpine[
                app.readerSpineIndex
            ].title;
    }

    return app.readerTitle;
}

void loadingFailed(
    App& app,
    const std::string& error
) {
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
    Screen returnScreen
) {
    if (
        spineIndex < 0 ||
        spineIndex >=
            static_cast<int>(
                app.readerSpine.size()
            )
    ) {
        return;
    }

    app.pendingSpineIndex =
        spineIndex;

    app.pendingPosition =
        position;

    app.pendingRestorePage =
        restorePage;

    app.pendingSearchDirection =
        searchDirection >= 0
        ? 1
        : -1;

    app.pendingDocument =
        ChapterDocument{};

    app.loadingStage =
        LoadingStage::LoadDocument;

    app.loadingReturnScreen =
        returnScreen;

    app.loadingMessage =
        "Loading section...";

    app.loadingDetail =
        app.readerSpine[
            spineIndex
        ].title;

    app.loadingWarmupFrames =
        0;

    app.screen =
        Screen::Loading;

    clearTextCache(app);
}

void finishSearchWithoutResult(
    App& app
) {
    if (
        app.loadingReturnScreen ==
            Screen::Reader &&
        !app.readerPages.empty() &&
        app.readerSpineIndex >= 0
    ) {
        app.pendingDocument =
            ChapterDocument{};

        app.pendingSpineIndex =
            -1;

        app.loadingStage =
            LoadingStage::None;

        app.loadingMessage.clear();
        app.loadingDetail.clear();

        app.screen =
            Screen::Reader;

        clearTextCache(app);

        return;
    }

    loadingFailed(
        app,
        "No supported reading content found"
    );
}

void skipPendingSpineItem(
    App& app,
    const std::string& reason
) {
    debugLog(
        "Skipping spine item %d: %s",
        app.pendingSpineIndex + 1,
        reason.c_str()
    );

    int nextIndex =
        app.pendingSpineIndex
        + app.pendingSearchDirection;

    if (
        nextIndex < 0 ||
        nextIndex >=
            static_cast<int>(
                app.readerSpine.size()
            )
    ) {
        finishSearchWithoutResult(app);
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
        app.loadingReturnScreen
    );
}

void beginOpenSelectedBook(
    App& app
) {
    if (
        app.books.empty() ||
        app.selected < 0 ||
        app.selected >=
            static_cast<int>(
                app.books.size()
            )
    ) {
        return;
    }

    const Book& book =
        app.books[
            app.selected
        ];

    if (!book.valid) {
        return;
    }

    destroyReaderImage(app);
    clearTextCache(app);

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

    app.readerActiveTocIndex =
        -1;

    app.tocSelected =
        0;

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
    std::vector<DocumentPage>&& pages
) {
    if (
        pages.empty()
    ) {
        skipPendingSpineItem(
            app,
            "Document produced no pages"
        );

        return;
    }

    app.readerPages =
        std::move(pages);

    app.readerSpineIndex =
        app.pendingSpineIndex;

    app.readerActiveTocIndex =
        activeTocIndexForSpine(
            app,
            app.readerSpineIndex
        );

    switch (
        app.pendingPosition
    ) {
        case DocumentPosition::Start:
            app.readerPageIndex =
                0;
            break;

        case DocumentPosition::End:
            app.readerPageIndex =
                static_cast<int>(
                    app.readerPages.size()
                )
                - 1;
            break;

        case DocumentPosition::Restore:
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

    app.tocSelected =
        app.readerActiveTocIndex >= 0
        ? app.readerActiveTocIndex
        : 0;

    app.pendingDocument =
        ChapterDocument{};

    app.pendingSpineIndex =
        -1;

    app.loadingStage =
        LoadingStage::None;

    app.loadingMessage.clear();
    app.loadingDetail.clear();

    app.screen =
        Screen::Reader;

    clearTextCache(app);

    loadCurrentPageImage(app);

    debugLog(
        "DISPLAY: spine=%d/%zu page=%d/%zu title=%s",
        app.readerSpineIndex + 1,
        app.readerSpine.size(),
        app.readerPageIndex + 1,
        app.readerPages.size(),
        currentSectionTitle(
            app
        ).c_str()
    );
}

void processLoadingStage(
    App& app
) {
    if (
        app.screen !=
            Screen::Loading ||
        app.loadingStage ==
            LoadingStage::Error
    ) {
        return;
    }

    if (
        app.loadingWarmupFrames < 2
    ) {
        ++app.loadingWarmupFrames;
        return;
    }

    if (
        app.loadingStage ==
        LoadingStage::OpenStructure
    ) {
        if (
            app.currentBookIndex < 0 ||
            app.currentBookIndex >=
                static_cast<int>(
                    app.books.size()
                )
        ) {
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
            loadReaderStructure(book);

        if (!structure.valid) {
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
        ) {
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
            DocumentPosition::Restore,
            progress.pageIndex,
            1,
            Screen::Library
        );

        return;
    }

    if (
        app.loadingStage ==
        LoadingStage::LoadDocument
    ) {
        if (
            app.currentBookIndex < 0 ||
            app.currentBookIndex >=
                static_cast<int>(
                    app.books.size()
                )
        ) {
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
        ) {
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

        if (!document.valid) {
            if (
                document.skippable
            ) {
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
            std::move(document);

        app.loadingStage =
            LoadingStage::LayoutDocument;

        app.loadingMessage =
            "Preparing pages...";

        app.loadingDetail =
            app.readerSpine[
                app.pendingSpineIndex
            ].title;

        app.loadingWarmupFrames =
            0;

        return;
    }

    if (
        app.loadingStage ==
        LoadingStage::LayoutDocument
    ) {
        std::vector<DocumentPage> pages =
            layoutDocument(
                app,
                app.pendingDocument
            );

        debugLog(
            "LAYOUT: %s -> %zu pages",
            app.pendingDocument
                .path.c_str(),
            pages.size()
        );

        finishDocument(
            app,
            std::move(pages)
        );
    }
}

void closeReader(
    App& app
) {
    rememberCurrentProgress(app);
    destroyReaderImage(app);
    clearTextCache(app);

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

    app.readerActiveTocIndex =
        -1;

    app.tocSelected =
        0;

    app.pendingSpineIndex =
        -1;

    app.pendingDocument =
        ChapterDocument{};
}

void navigateToAdjacentDocument(
    App& app,
    int direction
) {
    int target =
        app.readerSpineIndex
        + direction;

    if (
        target < 0 ||
        target >=
            static_cast<int>(
                app.readerSpine.size()
            )
    ) {
        return;
    }

    rememberCurrentProgress(app);

    scheduleDocumentLoad(
        app,
        target,
        direction > 0
            ? DocumentPosition::Start
            : DocumentPosition::End,
        0,
        direction,
        Screen::Reader
    );
}

void setReaderPage(
    App& app,
    int newPage
) {
    if (
        newPage < 0 ||
        newPage >=
            static_cast<int>(
                app.readerPages.size()
            )
    ) {
        return;
    }

    app.readerPageIndex =
        newPage;

    clearTextCache(app);

    loadCurrentPageImage(app);
}

void moveReaderByPage(
    App& app,
    int direction
) {
    if (
        app.readerPages.empty()
    ) {
        return;
    }

    int target =
        app.readerPageIndex
        + direction;

    if (
        target >= 0 &&
        target <
            static_cast<int>(
                app.readerPages.size()
            )
    ) {
        setReaderPage(
            app,
            target
        );

        rememberCurrentProgress(app);

        return;
    }

    navigateToAdjacentDocument(
        app,
        direction
    );
}

void openToc(
    App& app
) {
    if (
        app.readerToc.empty()
    ) {
        return;
    }

    app.readerActiveTocIndex =
        activeTocIndexForSpine(
            app,
            app.readerSpineIndex
        );

    app.tocSelected =
        app.readerActiveTocIndex >= 0
        ? app.readerActiveTocIndex
        : 0;

    app.screen =
        Screen::Toc;

    clearTextCache(app);
}

void closeToc(
    App& app
) {
    app.screen =
        Screen::Reader;

    clearTextCache(app);
}

void jumpToTocSelection(
    App& app
) {
    if (
        app.tocSelected < 0 ||
        app.tocSelected >=
            static_cast<int>(
                app.readerToc.size()
            )
    ) {
        return;
    }

    const TocEntry& target =
        app.readerToc[
            app.tocSelected
        ];

    if (
        target.spineIndex < 0
    ) {
        return;
    }

    rememberCurrentProgress(app);

    int direction =
        target.spineIndex <
            app.readerSpineIndex
        ? -1
        : 1;

    if (
        !target.fragment.empty()
    ) {
        debugLog(
            "TOC fragment preserved but not positioned yet: %s#%s",
            target.path.c_str(),
            target.fragment.c_str()
        );
    }

    scheduleDocumentLoad(
        app,
        target.spineIndex,
        DocumentPosition::Start,
        0,
        direction,
        Screen::Reader
    );
}

void cancelLoading(
    App& app
) {
    app.loadingStage =
        LoadingStage::None;

    app.loadingMessage.clear();
    app.loadingDetail.clear();

    app.pendingDocument =
        ChapterDocument{};

    app.pendingSpineIndex =
        -1;

    if (
        app.loadingReturnScreen ==
            Screen::Reader &&
        !app.readerPages.empty()
    ) {
        app.screen =
            Screen::Reader;
    } else {
        app.screen =
            Screen::Library;
    }

    clearTextCache(app);
}

bool initialize(
    App& app
) {
    if (
        SDL_Init(
            SDL_INIT_VIDEO |
            SDL_INIT_GAMECONTROLLER
        ) != 0
    ) {
        return false;
    }

    resetDebugLog();

    debugLog(
        "SDL initialized"
    );

    int imageFlags =
        IMG_INIT_PNG |
        IMG_INIT_JPG;

    int initializedImageFlags =
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
    ) {
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

    if (!app.window) {
        return false;
    }

    app.renderer =
        SDL_CreateRenderer(
            app.window,
            -1,
            SDL_RENDERER_ACCELERATED
        );

    if (!app.renderer) {
        return false;
    }

    SDL_RenderSetLogicalSize(
        app.renderer,
        SCREEN_WIDTH,
        SCREEN_HEIGHT
    );

    app.fontNormal =
        loadFont(17);

    app.fontSmall =
        loadFont(12);

    app.fontSmallItalic =
        loadFont(12);

    app.fontReader =
        loadFont(14);

    app.fontReaderItalic =
        loadFont(14);

    app.fontHeading =
        loadFont(16);

    if (
        app.fontSmallItalic
    ) {
        TTF_SetFontStyle(
            app.fontSmallItalic,
            TTF_STYLE_ITALIC
        );
    }

    if (
        app.fontReaderItalic
    ) {
        TTF_SetFontStyle(
            app.fontReaderItalic,
            TTF_STYLE_ITALIC
        );
    }

    if (
        app.fontHeading
    ) {
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
    ) {
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
    ) {
        if (
            !SDL_IsGameController(i)
        ) {
            continue;
        }

        app.controller =
            SDL_GameControllerOpen(i);

        if (
            app.controller
        ) {
            break;
        }
    }

    loadProgress(app);

    app.books =
        scanBooks();

    debugLog(
        "Found %zu EPUB(s)",
        app.books.size()
    );

    return true;
}

void handleLibraryButton(
    App& app,
    Uint8 button
) {
    switch (button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            if (
                !app.books.empty()
            ) {
                --app.selected;

                if (
                    app.selected < 0
                ) {
                    app.selected =
                        static_cast<int>(
                            app.books.size()
                        )
                        - 1;
                }
            }
            break;

        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            if (
                !app.books.empty()
            ) {
                ++app.selected;

                if (
                    app.selected >=
                    static_cast<int>(
                        app.books.size()
                    )
                ) {
                    app.selected = 0;
                }
            }
            break;

        case SDL_CONTROLLER_BUTTON_A:
            beginOpenSelectedBook(app);
            break;

        case SDL_CONTROLLER_BUTTON_START:
            app.running = false;
            break;

        default:
            break;
    }
}

void handleLoadingButton(
    App& app,
    Uint8 button
) {
    if (
        button ==
        SDL_CONTROLLER_BUTTON_B
    ) {
        cancelLoading(app);
    } else if (
        button ==
        SDL_CONTROLLER_BUTTON_START
    ) {
        rememberCurrentProgress(app);
        app.running = false;
    }
}

void handleReaderButton(
    App& app,
    Uint8 button
) {
    switch (button) {
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            moveReaderByPage(
                app,
                -1
            );
            break;

        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            moveReaderByPage(
                app,
                1
            );
            break;

        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            moveReaderByPage(
                app,
                -1
            );
            break;

        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            moveReaderByPage(
                app,
                1
            );
            break;

        case SDL_CONTROLLER_BUTTON_Y:
            openToc(app);
            break;

        case SDL_CONTROLLER_BUTTON_B:
            closeReader(app);
            break;

        case SDL_CONTROLLER_BUTTON_START:
            rememberCurrentProgress(app);
            app.running = false;
            break;

        default:
            break;
    }
}

void handleTocButton(
    App& app,
    Uint8 button
) {
    switch (button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            if (
                !app.readerToc.empty()
            ) {
                --app.tocSelected;

                if (
                    app.tocSelected < 0
                ) {
                    app.tocSelected =
                        static_cast<int>(
                            app.readerToc.size()
                        )
                        - 1;
                }
            }
            break;

        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            if (
                !app.readerToc.empty()
            ) {
                ++app.tocSelected;

                if (
                    app.tocSelected >=
                    static_cast<int>(
                        app.readerToc.size()
                    )
                ) {
                    app.tocSelected = 0;
                }
            }
            break;

        case SDL_CONTROLLER_BUTTON_A:
            jumpToTocSelection(app);
            break;

        case SDL_CONTROLLER_BUTTON_B:
        case SDL_CONTROLLER_BUTTON_Y:
            closeToc(app);
            break;

        case SDL_CONTROLLER_BUTTON_START:
            rememberCurrentProgress(app);
            app.running = false;
            break;

        default:
            break;
    }
}

void handleInput(
    App& app
) {
    SDL_Event event;

    while (
        SDL_PollEvent(
            &event
        )
    ) {
        if (
            event.type ==
            SDL_QUIT
        ) {
            rememberCurrentProgress(app);
            app.running = false;
            continue;
        }

        if (
            event.type ==
            SDL_CONTROLLERDEVICEADDED
        ) {
            if (
                !app.controller
            ) {
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
        ) {
            continue;
        }

        switch (
            app.screen
        ) {
            case Screen::Library:
                handleLibraryButton(
                    app,
                    event.cbutton.button
                );
                break;

            case Screen::Loading:
                handleLoadingButton(
                    app,
                    event.cbutton.button
                );
                break;

            case Screen::Reader:
                handleReaderButton(
                    app,
                    event.cbutton.button
                );
                break;

            case Screen::Toc:
                handleTocButton(
                    app,
                    event.cbutton.button
                );
                break;
        }
    }
}

void renderLibrary(
    App& app
) {
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
    ) {
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
    } else {
        constexpr int firstY =
            57;

        constexpr int rowHeight =
            43;

        constexpr int visibleBooks =
            4;

        int firstBook =
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
        ) {
            int index =
                firstBook
                + row;

            if (
                index >=
                static_cast<int>(
                    app.books.size()
                )
            ) {
                break;
            }

            int y =
                firstY
                + row
                * rowHeight;

            const Book& book =
                app.books[index];

            bool isSelected =
                index ==
                app.selected;

            if (
                isSelected
            ) {
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

            std::string subtitle =
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
    App& app
) {
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
        app.currentBookIndex >= 0 &&
        app.currentBookIndex <
            static_cast<int>(
                app.books.size()
            )
    ) {
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
            430
        ),
        24,
        78,
        muted
    );

    bool isError =
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
        fitTextToWidth(
            app.fontSmall,
            app.loadingDetail,
            430
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
        248,
        muted
    );

    SDL_RenderPresent(
        app.renderer
    );
}

void renderReader(
    App& app
) {
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

    std::string logicalIndicator;

    if (
        app.readerActiveTocIndex >= 0 &&
        app.readerActiveTocIndex <
            static_cast<int>(
                app.readerToc.size()
            )
    ) {
        logicalIndicator =
            std::to_string(
                app.readerActiveTocIndex + 1
            )
            + "/"
            + std::to_string(
                app.readerToc.size()
            );
    } else {
        logicalIndicator =
            std::to_string(
                app.readerSpineIndex + 1
            )
            + "/"
            + std::to_string(
                app.readerSpine.size()
            );
    }

    int indicatorWidth =
        textPixelWidth(
            app.fontSmall,
            logicalIndicator
        );

    int indicatorX =
        SCREEN_WIDTH
        - 16
        - indicatorWidth;

    int titleMaxWidth =
        std::max(
            40,
            indicatorX - 28
        );

    drawText(
        app,
        app.fontSmall,
        fitTextToWidth(
            app.fontSmall,
            currentSectionTitle(app),
            titleMaxWidth
        ),
        16,
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
    ) {
        const DocumentPage& page =
            app.readerPages[
                app.readerPageIndex
            ];

        if (
            page.kind ==
            PageKind::Text
        ) {
            int y =
                READER_TOP;

            for (
                const TextLine& line :
                page.lines
            ) {
                if (
                    !line.text.empty()
                ) {
                    TTF_Font* lineFont =
                        readerFontForStyle(
                            app,
                            line.style
                        );

                    if (
                        line.centered
                    ) {
                        drawCenteredText(
                            app,
                            lineFont,
                            line.text,
                            y,
                            text
                        );
                    } else {
                        drawText(
                            app,
                            lineFont,
                            line.text,
                            TEXT_LEFT,
                            y,
                            text
                        );
                    }
                }

                y +=
                    READER_LINE_HEIGHT;
            }
        } else {
            int imageAreaHeight =
                imageAreaHeightForPage(
                    page
                );

            int imageAreaTop =
                43;

            if (
                app.readerImageTexture
            ) {
                SDL_Rect destination
                {
                    TEXT_LEFT
                        + (
                            TEXT_WIDTH
                            - app.readerImageWidth
                        ) / 2,

                    imageAreaTop
                        + (
                            imageAreaHeight
                            - app.readerImageHeight
                        ) / 2,

                    app.readerImageWidth,
                    app.readerImageHeight
                };

                SDL_RenderCopy(
                    app.renderer,
                    app.readerImageTexture,
                    nullptr,
                    &destination
                );
            } else {
                drawCenteredText(
                    app,
                    app.fontSmall,
                    app.readerImageError.empty()
                        ? "Image unavailable"
                        : app.readerImageError,
                    imageAreaTop
                        + imageAreaHeight / 2,
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
            ) {
                if (
                    !line.text.empty()
                ) {
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

    std::string pageProgress =
        "Page "
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

    drawText(
        app,
        app.fontSmall,
        "L/R Page  TRIANGLE Index  O Back",
        14,
        253,
        muted
    );

    int progressWidth =
        textPixelWidth(
            app.fontSmall,
            pageProgress
        );

    drawText(
        app,
        app.fontSmall,
        pageProgress,
        SCREEN_WIDTH
            - 14
            - progressWidth,
        253,
        muted
    );

    SDL_RenderPresent(
        app.renderer
    );
}

void renderToc(
    App& app
) {
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
        SCREEN_WIDTH,
        45
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

    if (
        app.readerToc.empty()
    ) {
        drawText(
            app,
            app.fontNormal,
            "No chapters",
            175,
            120,
            text
        );
    } else {
        int firstItem =
            app.tocSelected >=
                TOC_VISIBLE_ITEMS
            ? app.tocSelected
                - TOC_VISIBLE_ITEMS
                + 1
            : 0;

        int maxFirst =
            std::max(
                0,
                static_cast<int>(
                    app.readerToc.size()
                )
                - TOC_VISIBLE_ITEMS
            );

        firstItem =
            std::clamp(
                firstItem,
                0,
                maxFirst
            );

        for (
            int row = 0;
            row < TOC_VISIBLE_ITEMS;
            ++row
        ) {
            int index =
                firstItem
                + row;

            if (
                index >=
                static_cast<int>(
                    app.readerToc.size()
                )
            ) {
                break;
            }

            int y =
                53
                + row
                * TOC_ROW_HEIGHT;

            bool isSelected =
                index ==
                app.tocSelected;

            bool isCurrent =
                index ==
                app.readerActiveTocIndex;

            if (
                isSelected
            ) {
                SDL_Rect selectionRect
                {
                    12,
                    y - 3,
                    456,
                    24
                };

                SDL_SetRenderDrawColor(
                    app.renderer,
                    selection.r,
                    selection.g,
                    selection.b,
                    selection.a
                );

                SDL_RenderFillRect(
                    app.renderer,
                    &selectionRect
                );
            }

            std::string number =
                std::to_string(
                    index + 1
                )
                + "/"
                + std::to_string(
                    app.readerToc.size()
                );

            int numberWidth =
                textPixelWidth(
                    app.fontSmall,
                    number
                );

            int numberX =
                SCREEN_WIDTH
                - 22
                - numberWidth;

            std::string title =
                app.readerToc[
                    index
                ].title;

            if (
                isCurrent
            ) {
                title =
                    "* "
                    + title;
            }

            title =
                fitTextToWidth(
                    app.fontSmall,
                    title,
                    numberX - 34
                );

            drawText(
                app,
                app.fontSmall,
                title,
                22,
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
        "X Jump   O Back   TRIANGLE Close",
        14,
        249,
        muted
    );

    SDL_RenderPresent(
        app.renderer
    );
}

void render(
    App& app
) {
    switch (
        app.screen
    ) {
        case Screen::Library:
            renderLibrary(app);
            break;

        case Screen::Loading:
            renderLoading(app);
            break;

        case Screen::Reader:
            renderReader(app);
            break;

        case Screen::Toc:
            renderToc(app);
            break;
    }
}

void shutdown(
    App& app
) {
    rememberCurrentProgress(app);
    destroyReaderImage(app);
    clearTextCache(app);

    if (
        app.controller
    ) {
        SDL_GameControllerClose(
            app.controller
        );
    }

    if (
        app.fontHeading
    ) {
        TTF_CloseFont(
            app.fontHeading
        );
    }

    if (
        app.fontReaderItalic
    ) {
        TTF_CloseFont(
            app.fontReaderItalic
        );
    }

    if (
        app.fontReader
    ) {
        TTF_CloseFont(
            app.fontReader
        );
    }

    if (
        app.fontSmallItalic
    ) {
        TTF_CloseFont(
            app.fontSmallItalic
        );
    }

    if (
        app.fontSmall
    ) {
        TTF_CloseFont(
            app.fontSmall
        );
    }

    if (
        app.fontNormal
    ) {
        TTF_CloseFont(
            app.fontNormal
        );
    }

    if (
        app.renderer
    ) {
        SDL_DestroyRenderer(
            app.renderer
        );
    }

    if (
        app.window
    ) {
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
    char* argv[]
) {
    (void)argc;
    (void)argv;

    App app;

    if (
        !initialize(app)
    ) {
        shutdown(app);
        return 1;
    }

    while (
        app.running
    ) {
        handleInput(app);

        render(app);

        if (
            app.screen ==
            Screen::Loading
        ) {
            processLoadingStage(app);
        }

        SDL_Delay(16);
    }

    shutdown(app);

    return 0;
}
