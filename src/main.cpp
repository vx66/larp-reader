#include <SDL.h>
#include <SDL_ttf.h>

#include <tinyxml2.h>
#include <zip.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

constexpr int SCREEN_WIDTH = 480;
constexpr int SCREEN_HEIGHT = 272;

constexpr const char* BOOKS_PATH = "ms0:/BOOKS";

constexpr zip_uint64_t MAX_XML_SIZE =
    2 * 1024 * 1024;

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

struct App
{
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;

    TTF_Font* fontNormal = nullptr;
    TTF_Font* fontSmall = nullptr;

    SDL_GameController* controller = nullptr;

    std::vector<Book> books;

    std::unordered_map<
        std::string,
        CachedText
    > textCache;

    int selected = 0;
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

const char* localName(const char* name)
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

    zip_stat_init(&statInfo);

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

    zip_int64_t totalRead = 0;

    while (
        totalRead <
        static_cast<zip_int64_t>(
            statInfo.size
        )
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

        totalRead += readNow;
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
        Book book =
            loadBook(filename);

        std::printf(
            "EPUB: %s\n",
            filename.c_str()
        );

        if (book.valid)
        {
            std::printf(
                "  title : %s\n",
                book.title.c_str()
            );

            std::printf(
                "  author: %s\n",
                book.author.c_str()
            );

            std::printf(
                "  opf   : %s\n",
                book.opfPath.c_str()
            );
        }
        else
        {
            std::printf(
                "  ERROR : %s\n",
                book.error.c_str()
            );
        }

        books.push_back(
            std::move(book)
        );
    }

    return books;
}

TTF_Font* loadFont(int size)
{
    const char* paths[] =
    {
        "assets/fonts/NotoSans-Regular.ttf",

        "ms0:/PSP/GAME/DeadReader/"
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
        SDL_FreeSurface(surface);
        return nullptr;
    }

    CachedText cached;

    cached.texture = texture;
    cached.width = surface->w;
    cached.height = surface->h;

    SDL_FreeSurface(surface);

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

void clearTextCache(App& app)
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

bool initialize(App& app)
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
            "DeadReader",
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

    if (
        !app.fontNormal ||
        !app.fontSmall
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
        "DeadReader: found %zu EPUB(s)\n",
        app.books.size()
    );

    return true;
}

void handleInput(App& app)
{
    SDL_Event event;

    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_QUIT)
        {
            app.running = false;
            continue;
        }

        if (
            event.type !=
            SDL_CONTROLLERBUTTONDOWN
        )
        {
            continue;
        }

        switch (event.cbutton.button)
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

            case SDL_CONTROLLER_BUTTON_START:
            {
                app.running = false;
                break;
            }

            default:
                break;
        }
    }
}

void render(App& app)
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
        "DeadReader",
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

        int visibleIndex = 0;

        for (
            std::size_t i = 0;
            i < app.books.size();
            ++i
        )
        {
            const int y =
                firstY +
                visibleIndex * rowHeight;

            if (y > 215)
                break;

            const Book& book =
                app.books[i];

            const bool isSelected =
                static_cast<int>(i) ==
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

            ++visibleIndex;
        }
    }

    drawText(
        app,
        app.fontSmall,
        "UP/DOWN  Select",
        14,
        249,
        muted
    );

    drawText(
        app,
        app.fontSmall,
        "START  Exit",
        394,
        249,
        muted
    );

    SDL_RenderPresent(
        app.renderer
    );
}

void shutdown(App& app)
{
    clearTextCache(app);

    if (app.controller)
        SDL_GameControllerClose(
            app.controller
        );

    if (app.fontSmall)
        TTF_CloseFont(
            app.fontSmall
        );

    if (app.fontNormal)
        TTF_CloseFont(
            app.fontNormal
        );

    if (app.renderer)
        SDL_DestroyRenderer(
            app.renderer
        );

    if (app.window)
        SDL_DestroyWindow(
            app.window
        );

    TTF_Quit();
    SDL_Quit();
}

}

int main(int argc, char* argv[])
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
