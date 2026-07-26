//AUDIO MODE
#define USE_MCI 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <windows.h>


// CONSTANTS
#define MAX_TITLE      80
#define MAX_ARTIST     50
#define MAX_GENRE      30
#define MAX_STACK      50
#define MAX_QUEUE      50
#define MAX_SONGS      100
#define MAX_FAVORITES  50
#define PROGRESS_WIDTH 36   /* characters wide for the bar */

/* CONSOLE COLORS (Windows) */
#define COL_RESET   7
#define COL_CYAN    11
#define COL_YELLOW  14
#define COL_GREEN   10
#define COL_RED     12
#define COL_MAGENTA 13
#define COL_WHITE   15
#define COL_BLUE    9
#define COL_GRAY    8

/*PLAYBACK STATE */
typedef enum {
    PB_STOPPED = 0,
    PB_PLAYING,
    PB_PAUSED
} PlaybackState;

/*SONG STRUCTURE*/
typedef struct Song {
    int    id;
    char   title[MAX_TITLE];
    char   artist[MAX_ARTIST];
    char   genre[MAX_GENRE];
    int    duration;       /* seconds */
    int    playCount;
    char   filePath[200];
    struct Song *prev;
    struct Song *next;
} Song;

/* STACK: Recently Played (LIFO) */
typedef struct {
    Song *data[MAX_STACK];
    int   top;
} RecentStack;

/*QUEUE: Upcoming Songs (FIFO)*/
typedef struct {
    Song *data[MAX_QUEUE];
    int   front, rear, size;
} UpcomingQueue;

/*GLOBAL STATE */
Song         *head           = NULL;
Song         *tail           = NULL;
Song         *nowPlaying     = NULL;
RecentStack   recentStack;
UpcomingQueue upcomingQueue;
Song         *favorites[MAX_FAVORITES];
int           favCount       = 0;
int           totalSongs     = 0;
int           repeatMode     = 0;   /* 0=off, 1=one, 2=all */
int           shuffleMode    = 0;
int           songIdCounter  = 1;

/* Playback engine state */
PlaybackState pbState        = PB_STOPPED;
int           pbElapsed      = 0;   /* seconds elapsed (simulated) */

#if USE_SDL
Mix_Music    *gMusic         = NULL;
int           sdlReady       = 0;
#endif

/* UTILITY HELPERS*/
void setColor(int color) {
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
}
void resetColor() { setColor(COL_RESET); }

void clearScreen() { system("cls"); }

void printLine(char ch, int len, int color) {
    setColor(color);
    for (int i = 0; i < len; i++) putchar(ch);
    putchar('\n');
    resetColor();
}

void printCentered(const char *text, int width, int color) {
    int pad = (width - (int)strlen(text)) / 2;
    setColor(color);
    for (int i = 0; i < pad; i++) putchar(' ');
    printf("%s\n", text);
    resetColor();
}

void pausePrompt() {
    setColor(COL_GRAY);
    printf("\n  Press ENTER to continue...");
    resetColor();
    getchar();
    getchar();
}

char *formatDuration(int sec, char *buf) {
    sprintf(buf, "%d:%02d", sec / 60, sec % 60);
    return buf;
}

/* Hide / show cursor (reduces flicker during bar animation) */
void hideCursor() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO ci = { 1, FALSE };
    SetConsoleCursorInfo(h, &ci);
}
void showCursor() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO ci = { 1, TRUE };
    SetConsoleCursorInfo(h, &ci);
}

/*STACK OPERATIONS  (O(1) push/pop)*/
void stackInit(RecentStack *s) { s->top = -1; }
int  stackFull(RecentStack *s) { return s->top == MAX_STACK - 1; }
int  stackEmpty(RecentStack *s){ return s->top == -1; }

void stackPush(RecentStack *s, Song *song) {
    if (stackFull(s)) {
        for (int i = 0; i < MAX_STACK - 1; i++)
            s->data[i] = s->data[i + 1];
        s->data[MAX_STACK - 1] = song;
    } else {
        s->data[++(s->top)] = song;
    }
}

Song *stackPop(RecentStack *s) {
    if (stackEmpty(s)) return NULL;
    return s->data[(s->top)--];
}

Song *stackPeek(RecentStack *s) {
    if (stackEmpty(s)) return NULL;
    return s->data[s->top];
}

/*QUEUE OPERATIONS  (O(1) enqueue/dequeue)*/
void queueInit(UpcomingQueue *q) { q->front = q->rear = q->size = 0; }
int  queueFull(UpcomingQueue *q) { return q->size == MAX_QUEUE; }
int  queueEmpty(UpcomingQueue *q){ return q->size == 0; }

void enqueue(UpcomingQueue *q, Song *song) {
    if (queueFull(q)) { printf("  [!] Queue is full!\n"); return; }
    q->data[q->rear] = song;
    q->rear = (q->rear + 1) % MAX_QUEUE;
    q->size++;
}

Song *dequeue(UpcomingQueue *q) {
    if (queueEmpty(q)) return NULL;
    Song *s = q->data[q->front];
    q->front = (q->front + 1) % MAX_QUEUE;
    q->size--;
    return s;
}

/*DOUBLY LINKED LIST OPERATIONS*/
Song *createSong(const char *title, const char *artist,
                 const char *genre, int duration, const char *filePath) {
    Song *s = (Song *)malloc(sizeof(Song));
    if (!s) { printf("Memory error!\n"); exit(1); }
    s->id = songIdCounter++;
    s->playCount = 0;
    s->prev = s->next = NULL;
    strncpy(s->title,    title,    MAX_TITLE  - 1);
    strncpy(s->artist,   artist,   MAX_ARTIST - 1);
    strncpy(s->genre,    genre,    MAX_GENRE  - 1);
    strncpy(s->filePath, filePath, 199);
    s->filePath[199] = '\0';
    s->duration = duration;
    return s;
}

void addSong(const char *title, const char *artist,
             const char *genre, int duration, const char *filePath) {
    Song *s = createSong(title, artist, genre, duration, filePath);
    if (!head) { head = tail = s; }
    else { tail->next = s; s->prev = tail; tail = s; }
    totalSongs++;
}

void deleteSong(int id) {
    Song *cur = head;
    while (cur) {
        if (cur->id == id) {
            if (cur == nowPlaying) {
                printf("  [!] Cannot delete currently playing song.\n");
                return;
            }
            if (cur->prev) cur->prev->next = cur->next; else head = cur->next;
            if (cur->next) cur->next->prev = cur->prev; else tail = cur->prev;
            free(cur);
            totalSongs--;
            printf("  [OK] Song deleted.\n");
            return;
        }
        cur = cur->next;
    }
    printf("  [!] Song not found.\n");
}

Song *findSongById(int id) {
    Song *cur = head;
    while (cur) { if (cur->id == id) return cur; cur = cur->next; }
    return NULL;
}

Song *findSongByTitle(const char *title) {
    Song *cur = head;
    while (cur) {
        if (_stricmp(cur->title, title) == 0) return cur;
        cur = cur->next;
    }
    return NULL;
}

/*SHUFFLE  (Fisher-Yates)*/
Song *shuffleArr[MAX_SONGS];
int   shuffleIdx = 0, shuffleLen = 0;

void buildShuffleArray() {
    shuffleLen = 0;
    Song *cur = head;
    while (cur) { shuffleArr[shuffleLen++] = cur; cur = cur->next; }
    srand((unsigned)time(NULL));
    for (int i = shuffleLen - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        Song *tmp = shuffleArr[i]; shuffleArr[i] = shuffleArr[j]; shuffleArr[j] = tmp;
    }
    shuffleIdx = 0;
}

#include <mmsystem.h>

static int mciOpen = 0;

void audioInit() { }

void audioCleanup() {
    if (mciOpen) {
        mciSendString("stop mysong", NULL, 0, NULL);
        mciSendString("close mysong", NULL, 0, NULL);
        mciOpen = 0;
    }
}

void audioPlay(const char *fp)
{
    char cmd[1024];
    char err[256];
    MCIERROR res;

    if (mciOpen)
    {
        mciSendString("stop mysong", NULL, 0, NULL);
        mciSendString("close mysong", NULL, 0, NULL);
        mciOpen = 0;
    }

    char shortPath[MAX_PATH];

    DWORD len = GetShortPathNameA(fp, shortPath, MAX_PATH);
    if (len == 0)
    {
        setColor(COL_RED);
        printf("\n[MCI] File not found:\n%s\n", fp);
        resetColor();
        return;
    }

    snprintf(cmd, sizeof(cmd), "open \"%s\" alias mysong", shortPath);
    res = mciSendString(cmd, NULL, 0, NULL);

    if (res != 0)
    {
        mciGetErrorString(res, err, sizeof(err));

        setColor(COL_RED);
        printf("\n[MCI] Cannot open file.\nError: %s\n", err);
        resetColor();
        return;
    }

    mciOpen = 1;
    mciSendString("play mysong", NULL, 0, NULL);
}

void audioPause()  { if (mciOpen) mciSendString("pause mysong",  NULL, 0, NULL); }
void audioResume() { if (mciOpen) mciSendString("resume mysong", NULL, 0, NULL); }

void audioStop() {
    if (mciOpen) {
        mciSendString("stop mysong",  NULL, 0, NULL);
        mciSendString("close mysong", NULL, 0, NULL);
        mciOpen = 0;
    }
}

int audioIsPlaying() { return pbState == PB_PLAYING; }

void drawProgressBar(int elapsed, int total, PlaybackState state) {
    if (total <= 0) total = 1;
    float pct    = (float)elapsed / total;
    if (pct > 1.0f) pct = 1.0f;
    int   filled = (int)(pct * PROGRESS_WIDTH);

    char e[12], t[12];
    formatDuration(elapsed, e);
    formatDuration(total,   t);

    /* Carriage-return to overwrite the same line */
    printf("\r  ");

    /* Elapsed time */
    setColor(COL_CYAN);
    printf("%5s ", e);

    /* Bar brackets */
    setColor(COL_WHITE);
    printf("[");

    /* Filled portion (block character \xDB = ░ on CP437) */
    setColor(COL_GREEN);
    for (int i = 0; i < filled; i++) printf("\xDB");

    /* Unfilled portion */
    setColor(COL_GRAY);
    for (int i = filled; i < PROGRESS_WIDTH; i++) printf("-");

    /* Close bracket */
    setColor(COL_WHITE);
    printf("] ");

    /* Total time */
    setColor(COL_CYAN);
    printf("%s ", t);

    /* Percentage */
    setColor(COL_YELLOW);
    printf("%3.0f%%", pct * 100.0f);

    /* Paused indicator */
    if (state == PB_PAUSED) {
        setColor(COL_RED);
        printf("  [PAUSED] ");
    } else {
        printf("          "); /* blank out old [PAUSED] text */
    }

    resetColor();
    fflush(stdout);
}

/*NOW PLAYING BANNER*/
void printNowPlaying(Song *song) {
    char dur[16];
    formatDuration(song->duration, dur);

    printLine('=', 64, COL_CYAN);
    setColor(COL_MAGENTA);
    printf("  \xA0 NOW PLAYING\n");
    setColor(COL_WHITE);
    printf("  Title  : "); setColor(COL_YELLOW); printf("%s\n", song->title);
    setColor(COL_WHITE);
    printf("  Artist : "); setColor(COL_CYAN);   printf("%s\n", song->artist);
    setColor(COL_WHITE);
    printf("  Genre  : "); setColor(COL_MAGENTA);printf("%s\n", song->genre);
    setColor(COL_WHITE);
    printf("  Length : "); setColor(COL_GREEN);  printf("%s\n", dur);
    setColor(COL_WHITE);
    printf("  Plays  : "); setColor(COL_YELLOW); printf("%d", song->playCount);
    setColor(COL_GRAY);
    printf("  |  ");
    setColor(COL_WHITE); printf("Repeat: ");
    setColor(COL_CYAN);
    printf("%s", repeatMode == 0 ? "OFF" : repeatMode == 1 ? "ONE" : "ALL");
    setColor(COL_GRAY); printf("  |  ");
    setColor(COL_WHITE); printf("Shuffle: ");
    setColor(COL_CYAN);  printf("%s\n", shuffleMode ? "ON " : "OFF");
    printLine('=', 64, COL_CYAN);
}

/*PLAYBACK SCREEN*/
#include <conio.h>  /* _kbhit(), _getch() — Windows only */

void printPlaybackControls() {
    setColor(COL_GRAY);
    printf("\n  Controls: [P] Pause/Resume   [N] Next   [B] Back   [Q] Menu\n");
    printf("  ");
    resetColor();
}

char runPlaybackScreen() {
    if (!nowPlaying) return 'q';

    clearScreen();
    printNowPlaying(nowPlaying);
    printPlaybackControls();
    printf("\n");   /* blank line before the bar */

    hideCursor();

    int total   = nowPlaying->duration;
    int elapsed = pbElapsed;   /* resume from where we left off */
    int tickMs  = 250;         /* update every 250 ms */
    int ticksPerSec = 1000 / tickMs;
    int subTick = 0;

    char result = 'e';         /* assume song runs to end */

    while (1) {
        /* Draw bar */
        drawProgressBar(elapsed, total, pbState);

        /* Sleep one tick */
        Sleep(tickMs);

        /* Advance time only if actually playing */
        if (pbState == PB_PLAYING) {
            subTick++;
            if (subTick >= ticksPerSec) {
                subTick = 0;
                elapsed++;
                pbElapsed = elapsed;
            }
        }

        /* Check if song finished */
        if (elapsed >= total) {
            pbElapsed = 0;
            pbState   = PB_STOPPED;
            result    = 'e';
            break;
        }

        /* Check for key press (non-blocking) */
        if (_kbhit()) {
            int ch = _getch();
            /* Handle arrow/function keys (two-char sequences) */
            if (ch == 0 || ch == 224) { _getch(); continue; }

            ch = tolower(ch);
            if (ch == 'p') {
                /* Toggle pause / resume */
                if (pbState == PB_PLAYING) {
                    pbState = PB_PAUSED;
                    audioPause();
                } else if (pbState == PB_PAUSED) {
                    pbState = PB_PLAYING;
                    audioResume();
                }
            } else if (ch == 'n') {
                pbElapsed = 0;
                pbState   = PB_STOPPED;
                audioStop();
                result = 'n';
                break;
            } else if (ch == 'b') {
                pbElapsed = 0;
                pbState   = PB_STOPPED;
                audioStop();
                result = 'b';
                break;
            } else if (ch == 'q') {
                /* Do NOT stop audio — just return to menu */
                pbElapsed = elapsed;   /* remember position */
                result = 'q';
                break;
            }
        }
    }

    printf("\n");   /* move past the progress bar line */
    showCursor();
    return result;
}

/*FORWARD DECLARATIONS for navigation*/
void playSong(Song *song);
void playNext();
void playPrev();

/*PLAY A SONG*/
void playSong(Song *song) {
    if (!song) { printf("  [!] No song selected.\n"); return; }

    /* Push previous to recently-played stack */
    if (nowPlaying && nowPlaying != song)
        stackPush(&recentStack, nowPlaying);

    nowPlaying  = song;
    song->playCount++;
    pbElapsed   = 0;
    pbState     = PB_PLAYING;

    /* Start real audio */
    audioPlay(song->filePath);

    /* Auto-queue next song if queue empty */
    if (queueEmpty(&upcomingQueue) && song->next)
        enqueue(&upcomingQueue, song->next);

    /* Enter playback screen — handles keyboard + progress bar */
    char action = runPlaybackScreen();

    if      (action == 'n') playNext();
    else if (action == 'b') playPrev();
    else if (action == 'e') {
        /* Song ended naturally — obey repeat / queue */
        if (repeatMode == 1) { playSong(nowPlaying); }
        else                 { playNext(); }
    }
    /* 'q' — just return to menu, audio keeps going */
}

/*NAVIGATION: Next / Previous*/
void playNext() {
    if (!nowPlaying) { printf("  [!] No song is playing.\n"); return; }

    Song *next = NULL;
    if (!queueEmpty(&upcomingQueue)) {
        next = dequeue(&upcomingQueue);
    } else if (shuffleMode) {
        if (shuffleIdx >= shuffleLen) buildShuffleArray();
        next = shuffleArr[shuffleIdx++];
    } else if (repeatMode == 1) {
        next = nowPlaying;
    } else if (nowPlaying->next) {
        next = nowPlaying->next;
    } else if (repeatMode == 2) {
        next = head;
    } else {
        printf("  [!] End of playlist.\n"); return;
    }
    playSong(next);
}

void playPrev() {
    if (!nowPlaying) { printf("  [!] No song is playing.\n"); return; }

    Song *prev = stackPop(&recentStack);
    if (!prev && nowPlaying->prev) prev = nowPlaying->prev;
    if (!prev) { printf("  [!] No previous song.\n"); return; }

    stackPush(&recentStack, nowPlaying);
    nowPlaying = NULL;
    playSong(prev);
}

/*FAVORITES*/
int isInFavorites(Song *song) {
    for (int i = 0; i < favCount; i++) if (favorites[i] == song) return 1;
    return 0;
}

void addToFavorites(Song *song) {
    if (!song) return;
    if (favCount >= MAX_FAVORITES) { printf("  [!] Favorites full.\n"); return; }
    if (isInFavorites(song)) { printf("  [!] Already in favorites.\n"); return; }
    favorites[favCount++] = song;
    setColor(COL_GREEN);
    printf("  [OK] \"%s\" added to favorites.\n", song->title);
    resetColor();
}

void showFavorites() {
    char dur[16];
    printLine('=', 64, COL_YELLOW);
    printCentered("FAVORITES PLAYLIST", 64, COL_YELLOW);
    printLine('=', 64, COL_YELLOW);
    if (favCount == 0) {
        setColor(COL_GRAY);
        printf("  No favorites yet.\n");
        resetColor(); return;
    }
    setColor(COL_WHITE);
    printf("  %-4s  %-28s %-18s %s\n", "No.", "Title", "Artist", "Dur");
    printLine('-', 64, COL_GRAY);
    for (int i = 0; i < favCount; i++) {
        setColor(COL_YELLOW);
        printf("  %-4d  ", i + 1);
        setColor(COL_WHITE);
        printf("%-28s %-18s %s\n",
               favorites[i]->title, favorites[i]->artist,
               formatDuration(favorites[i]->duration, dur));
    }
    resetColor();
}

/*DISPLAY FULL PLAYLIST*/
void displayPlaylist() {
    char dur[16];
    printLine('=', 74, COL_CYAN);
    printCentered("FULL PLAYLIST", 74, COL_CYAN);
    printLine('=', 74, COL_CYAN);

    if (!head) {
        setColor(COL_GRAY); printf("  Playlist is empty.\n"); resetColor(); return;
    }

    setColor(COL_WHITE);
    printf("  %-4s %-28s %-18s %-12s %-6s %s\n",
           "ID", "Title", "Artist", "Genre", "Plays", "Dur");
    printLine('-', 74, COL_GRAY);

    Song *cur = head;
    while (cur) {
        if (cur == nowPlaying) setColor(COL_GREEN);
        else                   setColor(COL_WHITE);

        printf("  %-4d %-28s %-18s %-12s %-6d %s",
               cur->id, cur->title, cur->artist,
               cur->genre, cur->playCount,
               formatDuration(cur->duration, dur));

        if (cur == nowPlaying) {
            setColor(COL_MAGENTA);
            const char *stLabel = pbState == PB_PAUSED ? "  \xA7 PAUSED" : "  \xA7 PLAYING";
            printf("%s", stLabel);
        }
        if (isInFavorites(cur)) { setColor(COL_YELLOW); printf(" \x01"); }
        printf("\n");
        cur = cur->next;
    }
    setColor(COL_CYAN);
    printf("\n  Total: %d songs\n", totalSongs);
    resetColor();
}

/*RECENTLY PLAYED (Stack display)*/
void showRecentlyPlayed() {
    printLine('=', 64, COL_MAGENTA);
    printCentered("RECENTLY PLAYED", 64, COL_MAGENTA);
    printLine('=', 64, COL_MAGENTA);
    if (stackEmpty(&recentStack)) {
        setColor(COL_GRAY); printf("  Stack is empty.\n"); resetColor(); return;
    }
    setColor(COL_WHITE);
    printf("  %-4s %-30s %s\n", "Pos", "Title", "Artist");
    printLine('-', 64, COL_GRAY);
    for (int i = recentStack.top; i >= 0; i--) {
        setColor(i == recentStack.top ? COL_GREEN : COL_WHITE);
        printf("  %-4d %-30s %s\n",
               recentStack.top - i + 1,
               recentStack.data[i]->title,
               recentStack.data[i]->artist);
    }
    resetColor();
}

/*UPCOMING QUEUE (Queue display)*/
void showUpcomingQueue() {
    printLine('=', 64, COL_BLUE);
    printCentered("UPCOMING QUEUE", 64, COL_BLUE);
    printLine('=', 64, COL_BLUE);
    if (queueEmpty(&upcomingQueue)) {
        setColor(COL_GRAY); printf("  Queue is empty.\n"); resetColor(); return;
    }
    int idx = upcomingQueue.front, cnt = 1;
    for (int i = 0; i < upcomingQueue.size; i++) {
        Song *s = upcomingQueue.data[idx];
        setColor(i == 0 ? COL_CYAN : COL_WHITE);
        printf("  %d. %-30s %s\n", cnt++, s->title, s->artist);
        idx = (idx + 1) % MAX_QUEUE;
    }
    resetColor();
}

/*ANALYTICS: Most Played (selection sort — O(n^2))*/
void showMostPlayed(int topN) {
    if (!head) return;
    Song *arr[MAX_SONGS]; int n = 0;
    Song *cur = head;
    while (cur) { arr[n++] = cur; cur = cur->next; }

    for (int i = 0; i < n - 1; i++) {
        int mx = i;
        for (int j = i + 1; j < n; j++)
            if (arr[j]->playCount > arr[mx]->playCount) mx = j;
        Song *tmp = arr[i]; arr[i] = arr[mx]; arr[mx] = tmp;
    }

    if (topN > n) topN = n;
    printLine('=', 64, COL_YELLOW);
    printCentered("MOST PLAYED SONGS", 64, COL_YELLOW);
    printLine('=', 64, COL_YELLOW);
    printf("  %-4s %-28s %-18s %s\n", "Rank", "Title", "Artist", "Plays");
    printLine('-', 64, COL_GRAY);
    for (int i = 0; i < topN; i++) {
        setColor(i == 0 ? COL_GREEN : COL_WHITE);
        printf("  %-4d %-28s %-18s %d\n",
               i + 1, arr[i]->title, arr[i]->artist, arr[i]->playCount);
    }
    resetColor();
}

/*SEARCH: partial, case-insensitive (O(n))*/
void searchSongs(const char *keyword) {
    char dur[16]; int found = 0;
    printLine('=', 64, COL_GREEN);
    setColor(COL_GREEN);
    printf("  Search results for: \"%s\"\n", keyword);
    printLine('=', 64, COL_GREEN);

    char lkw[MAX_TITLE];
    strncpy(lkw, keyword, MAX_TITLE - 1);
    for (int i = 0; lkw[i]; i++) lkw[i] = (char)tolower((unsigned char)lkw[i]);

    Song *cur = head;
    while (cur) {
        char lt[MAX_TITLE], la[MAX_ARTIST];
        strncpy(lt, cur->title,  MAX_TITLE  - 1);
        strncpy(la, cur->artist, MAX_ARTIST - 1);
        for (int i = 0; lt[i]; i++) lt[i] = tolower(lt[i]);
        for (int i = 0; la[i]; i++) la[i] = tolower(la[i]);

        if (strstr(lt, lkw) || strstr(la, lkw)) {
            setColor(COL_WHITE);
            printf("  [ID:%d] %-28s %-18s %s\n",
                   cur->id, cur->title, cur->artist,
                   formatDuration(cur->duration, dur));
            found++;
        }
        cur = cur->next;
    }
    if (!found) { setColor(COL_RED); printf("  No songs found.\n"); }
    resetColor();
}

/*RECOMMENDATION ENGINE  (O(n) linear scan)*/
void recommendSongs() {
    if (!nowPlaying) {
        printf("  [!] Play a song first to get recommendations.\n"); return;
    }
    printLine('=', 64, COL_MAGENTA);
    setColor(COL_MAGENTA);
    printf("  Based on: \"%s\" (%s / %s)\n",
           nowPlaying->title, nowPlaying->artist, nowPlaying->genre);
    printLine('=', 64, COL_MAGENTA);

    char dur[16]; int found = 0;
    Song *cur = head;
    while (cur) {
        if (cur != nowPlaying &&
           (strcmp(cur->artist, nowPlaying->artist) == 0 ||
            strcmp(cur->genre,  nowPlaying->genre)  == 0)) {
            setColor(COL_WHITE);
            printf("  \xBB %-28s %-18s [%s]\n",
                   cur->title, cur->artist,
                   formatDuration(cur->duration, dur));
            found++;
        }
        cur = cur->next;
    }
    if (!found) { setColor(COL_GRAY); printf("  No recommendations found.\n"); }
    resetColor();
}

/*QUEUE A SONG*/
void queueSong(int id) {
    Song *s = findSongById(id);
    if (!s) { printf("  [!] Song not found.\n"); return; }
    enqueue(&upcomingQueue, s);
    setColor(COL_GREEN);
    printf("  [OK] \"%s\" added to queue.\n", s->title);
    resetColor();
}

/*TOGGLE MODES*/
void toggleRepeat() {
    repeatMode = (repeatMode + 1) % 3;
    const char *modes[] = { "OFF", "ONE", "ALL" };
    setColor(COL_CYAN);
    printf("  Repeat mode: %s\n", modes[repeatMode]);
    resetColor();
}

void toggleShuffle() {
    shuffleMode = !shuffleMode;
    if (shuffleMode) buildShuffleArray();
    setColor(COL_CYAN);
    printf("  Shuffle: %s\n", shuffleMode ? "ON" : "OFF");
    resetColor();
}

/*HEADER BANNER*/
void printHeader() {
    clearScreen();
    setColor(COL_CYAN);
    printf("  +============================================================+\n");
    printf("  |                                                            |\n");
    printf("  |            ");
    setColor(COL_WHITE);  printf("C");
    setColor(COL_CYAN);   printf(" ");
    setColor(COL_MAGENTA);printf("P");
    setColor(COL_WHITE);  printf("U");
    setColor(COL_YELLOW); printf("L");
    setColor(COL_GREEN);  printf("S");
    setColor(COL_CYAN);   printf("E");
    setColor(COL_CYAN);
    printf("   ~  DSA Music Player in C              |\n");
    printf("  |                                                            |\n");
    printf("  +============================================================+\n");
    resetColor();
}

void printNowPlayingMini() {
    if (!nowPlaying) {
        setColor(COL_GRAY);
        printf("  \xA7 Not playing anything  |  ");
    } else {
        /* State icon */
        setColor(pbState == PB_PAUSED ? COL_YELLOW : COL_GREEN);
        printf("  %s %-20s by %-15s  |  ",
               pbState == PB_PAUSED ? "\xA7 PAUSED:" : "\xA7 PLAYING:",
               nowPlaying->title, nowPlaying->artist);
    }
    setColor(COL_YELLOW);
    const char *rep[] = { "RPT:OFF", "RPT:ONE", "RPT:ALL" };
    printf("%s  SHF:%s\n", rep[repeatMode], shuffleMode ? "ON" : "OFF");
    resetColor();
}

/*MAIN MENU*/
void printMainMenu() {
    printHeader();
    printNowPlayingMini();
    printLine('-', 64, COL_GRAY);

    printf("\n");
    setColor(COL_CYAN);  printf("   PLAYBACK\n");
    setColor(COL_WHITE);
    printf("    1. Display Playlist         2. Play Song by ID\n");
    printf("    3. Next Song                4. Previous Song\n");
    printf("    5. Resume Playback Screen   6. Toggle Repeat\n");
    printf("    7. Toggle Shuffle\n");
    printf("\n");
    setColor(COL_CYAN);  printf("   QUEUE & HISTORY\n");
    setColor(COL_WHITE);
    printf("    8. Add Song to Queue        9. View Upcoming Queue\n");
    printf("   10. View Recently Played\n");
    printf("\n");
    setColor(COL_CYAN);  printf("   SEARCH & DISCOVER\n");
    setColor(COL_WHITE);
    printf("   11. Search Song             12. Get Recommendations\n");
    printf("   13. Most Played Analytics\n");
    printf("\n");
    setColor(COL_CYAN);  printf("   FAVORITES\n");
    setColor(COL_WHITE);
    printf("   14. Add to Favorites        15. View Favorites\n");
    printf("\n");
    setColor(COL_CYAN);  printf("   MANAGE\n");
    setColor(COL_WHITE);
    printf("   16. Add New Song            17. Delete Song\n");
    printf("\n");
    setColor(COL_RED);
    printf("    0. Exit\n\n");
    printLine('-', 64, COL_GRAY);
    setColor(COL_WHITE);
    printf("  Enter choice: ");
    resetColor();
}

/*LOAD DEFAULT SONGS*/

    void loadDefaultSongs() {
    addSong("Oorum Blood", "Abhyankkar", "Indian-Pop", 269,
        "songs\\oorum_blood.mp3");

    addSong("Aaya Sher", "Anirudh", "Folk", 213,
        "songs\\aaya_sher.mp3");

    addSong("Blinding Lights", "The Weeknd", "Pop", 200,
        "songs\\blinding_lights.mp3");

    addSong("Levitating", "Dua Lipa", "Pop", 204,
        "songs\\levitating.mp3");

    addSong("Believer", "Imagine Dragons", "Rock", 204,
        "songs\\believer.mp3");

    addSong("Star Wars", "Cantina Bands", "Jazz", 165,
        "songs\\star_wars.mp3");

    addSong("Starboy", "The Weekend", "R&B", 273,
            "songs\\starboy.mp3");

    addSong("They call him OG- Firestorm", "Silambarasan TR", "EDM", 246,
            "songs\\firestorm.mp3");
}

/*MAIN*/
int main(int argc, char *argv[])  {
    stackInit(&recentStack);
    queueInit(&upcomingQueue);
    srand((unsigned)time(NULL));
    audioInit();
    loadDefaultSongs();

    int  choice;
    char inputBuf[MAX_TITLE];

    while (1) {
        printMainMenu();
        if (scanf("%d", &choice) != 1) {
            while (getchar() != '\n');
            continue;
        }

        switch (choice) {

        /* ── 1. Display Playlist ── */
        case 1:
            clearScreen();
            displayPlaylist();
            pausePrompt();
            break;

        /* ── 2. Play Song by ID ── */
        case 2:
            clearScreen();
            displayPlaylist();
            setColor(COL_WHITE);
            printf("\n  Enter Song ID to play: ");
            resetColor();
            {
                int pid; scanf("%d", &pid);
                Song *s = findSongById(pid);
                if (s) playSong(s);
                else { setColor(COL_RED); printf("  [!] Song ID not found.\n"); resetColor(); pausePrompt(); }
            }
            break;

        /* ── 3. Next ── */
        case 3:
            clearScreen();
            playNext();
            break;

        /* ── 4. Previous ── */
        case 4:
            clearScreen();
            playPrev();
            break;

        /* ── 5. Resume Playback Screen ── */
        case 5:
            if (!nowPlaying) {
                setColor(COL_RED);
                printf("  [!] No song is currently loaded.\n");
                resetColor();
                pausePrompt();
            } else {
                /* Re-enter the playback screen for the current song */
                if (pbState == PB_STOPPED) pbState = PB_PLAYING;
                char action = runPlaybackScreen();
                if      (action == 'n') playNext();
                else if (action == 'b') playPrev();
                else if (action == 'e') {
                    if (repeatMode == 1) playSong(nowPlaying);
                    else                 playNext();
                }
            }
            break;

        /* ── 6. Toggle Repeat ── */
        case 6:
            clearScreen();
            toggleRepeat();
            pausePrompt();
            break;

        /* ── 7. Toggle Shuffle ── */
        case 7:
            clearScreen();
            toggleShuffle();
            pausePrompt();
            break;

        /* ── 8. Add to Queue ── */
        case 8:
            clearScreen();
            displayPlaylist();
            setColor(COL_WHITE);
            printf("\n  Enter Song ID to queue: ");
            resetColor();
            { int qid; scanf("%d", &qid); queueSong(qid); }
            pausePrompt();
            break;

        /* ── 9. View Queue ── */
        case 9:
            clearScreen();
            showUpcomingQueue();
            pausePrompt();
            break;

        /* ── 10. Recently Played ── */
        case 10:
            clearScreen();
            showRecentlyPlayed();
            pausePrompt();
            break;

        /* ── 11. Search ── */
        case 11:
            clearScreen();
            setColor(COL_WHITE);
            printf("  Enter title/artist keyword: ");
            resetColor();
            scanf(" %[^\n]", inputBuf);
            clearScreen();
            searchSongs(inputBuf);
            pausePrompt();
            break;

        /* ── 12. Recommendations ── */
        case 12:
            clearScreen();
            recommendSongs();
            pausePrompt();
            break;

        /* ── 13. Analytics ── */
        case 13:
            clearScreen();
            showMostPlayed(5);
            pausePrompt();
            break;

        /* ── 14. Add to Favorites ── */
        case 14:
            clearScreen();
            if (!nowPlaying) {
                setColor(COL_RED);
                printf("  [!] No song is currently playing.\n");
                resetColor();
            } else {
                addToFavorites(nowPlaying);
            }
            pausePrompt();
            break;

        /* ── 15. View Favorites ── */
        case 15:
            clearScreen();
            showFavorites();
            pausePrompt();
            break;

        /* ── 16. Add New Song ── */
        case 16: {
            clearScreen();
            printLine('=', 64, COL_CYAN);
            printCentered("ADD NEW SONG", 64, COL_CYAN);
            printLine('=', 64, COL_CYAN);
            char nt[MAX_TITLE], na[MAX_ARTIST], ng[MAX_GENRE], np[120]; int nd;
            printf("  Title    : "); scanf(" %[^\n]", nt);
            printf("  Artist   : "); scanf(" %[^\n]", na);
            printf("  Genre    : "); scanf(" %[^\n]", ng);
            printf("  Duration (seconds): "); scanf("%d", &nd);
            printf("  File path: "); scanf(" %[^\n]", np);
            addSong(nt, na, ng, nd, np);
            setColor(COL_GREEN);
            printf("\n  [OK] Song added! (ID: %d)\n", songIdCounter - 1);
            resetColor();
            pausePrompt();
            break;
        }

        /* ── 17. Delete Song ── */
        case 17:
            clearScreen();
            displayPlaylist();
            setColor(COL_WHITE);
            printf("\n  Enter Song ID to delete: ");
            resetColor();
            { int did; scanf("%d", &did); deleteSong(did); }
            pausePrompt();
            break;

        /* ── 0. Exit ── */
        case 0:
            clearScreen();
            audioStop();
            audioCleanup();
            printLine('=', 64, COL_CYAN);
            printCentered("Thanks for using C PULSE!", 64, COL_MAGENTA);
            printCentered("Goodbye! Keep the music alive.", 64, COL_YELLOW);
            printLine('=', 64, COL_CYAN);
            /* Free playlist memory */
            { Song *cur = head; while (cur) { Song *nx = cur->next; free(cur); cur = nx; } }
            return 0;

        default:
            setColor(COL_RED);
            printf("  [!] Invalid option. Try again.\n");
            resetColor();
            Sleep(800);
            break;
        }
    }
    return 0;
}
