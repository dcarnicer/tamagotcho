// The decks' two places and the microSD card; see Storage.h.
#include "platform/Storage.h"

#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>
#include <dirent.h>
#include <esp_heap_caps.h>
#include <ff.h>
#include <mbedtls/sha256.h>
#include <sd_diskio.h>
#include <sys/stat.h>
#include <vfs_api.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#include "board/Board.h"
#include "platform/FsQuiet.h"
#include "platform/FsSrsFile.h"
#include "platform/PackHash.h"

namespace tgc {
namespace storage {

namespace {

// Arduino's SD driver caps the clock at 25 MHz, and the pins go through the
// GPIO matrix, whose input path is good to about 26 MHz full duplex: 20 MHz
// leaves a margin (about 1-2 MB/s with this driver's 512-byte sectors).
constexpr uint32_t kCardHz = 20000000;
// Files open on the card at once: the loaded deck and one more (a download or
// upload temp, a deck hashed, peeked or probed, the move's temp, the progress
// copy). This framework's FatFs has 4 KB sector buffers (sized for the flash's
// wear levelling), one per open file plus one for the volume, so two files keep
// the card at about 13 KB of internal heap. Listings use readdir() and stat(),
// which open no file.
constexpr uint8_t kCardMaxFiles = 2;
constexpr size_t kCopyChunk = 4 * 1024;
constexpr size_t kReadBuffer = 512;  // one card sector

enum class Mount : uint8_t { Mounted, NoCard, NotFat };

// fs::SDFS whose begin() tells a card that did not answer from one with no FAT
// volume on it: SDFS::begin() is false for both, and the card type that tells
// them apart (set by the card's init, which f_mount() runs) is reset before it
// returns. The bus is the caller's (SPI.begin() with the board's pins).
class CardFs : public fs::SDFS {
public:
    CardFs() : fs::SDFS(fs::FSImplPtr(new VFSImpl())) {}

    Mount mount(uint8_t cs, SPIClass& spi) {
        if (_pdrv != 0xFF) return Mount::Mounted;
        _pdrv = sdcard_init(cs, &spi, kCardHz);
        if (_pdrv == 0xFF) return Mount::NoCard;  // no FatFs drive free (none is used elsewhere)
        if (sdcard_mount(_pdrv, kCardMount, kCardMaxFiles, false)) {
            _impl->mountpoint(kCardMount);
            return Mount::Mounted;
        }
        const sdcard_type_t type = sdcard_type(_pdrv);
        sdcard_unmount(_pdrv);
        sdcard_uninit(_pdrv);
        _pdrv = 0xFF;
        return (type == CARD_NONE || type == CARD_UNKNOWN) ? Mount::NoCard : Mount::NotFat;
    }

    // The FAT volume's size and free bytes (f_getfree(): the first call after a
    // mount reads the whole FAT when the card's FSINFO has no free count, a
    // few seconds on a 32 GB card; FatFs keeps the count from then on).
    bool space(uint64_t* total, uint64_t* freeB, uint8_t* fatType = nullptr) {
        if (_pdrv == 0xFF) return false;
        FATFS* fs = nullptr;
        DWORD freeClusters = 0;
        const char drv[3] = {static_cast<char>('0' + _pdrv), ':', 0};
        if (f_getfree(drv, &freeClusters, &fs) != FR_OK || !fs) return false;
#if FF_MAX_SS != FF_MIN_SS
        const uint64_t cluster = static_cast<uint64_t>(fs->csize) * fs->ssize;
#else
        const uint64_t cluster = static_cast<uint64_t>(fs->csize) * FF_MAX_SS;
#endif
        *total = static_cast<uint64_t>(fs->n_fatent - 2) * cluster;
        *freeB = static_cast<uint64_t>(freeClusters) * cluster;
        if (fatType) *fatType = fs->fs_type;
        return true;
    }
};

CardFs gCard;
CardState gState = CardState::NoSlot;
tgc::board::SdPins gPins;

const char* cardTypeName(sdcard_type_t t) {
    switch (t) {
        case CARD_MMC: return "MMC";
        case CARD_SD: return "SDSC";
        case CARD_SDHC: return "SDHC/SDXC";
        default: return "unknown";
    }
}

const char* fatName(uint8_t t) {
    switch (t) {
        case FS_FAT12: return "FAT12";
        case FS_FAT16: return "FAT16";
        case FS_FAT32: return "FAT32";
        default: return "FAT";
    }
}

// One mount attempt, logged with its time and the heap it took ("[sd] ...",
// as the "[net]" heap lines around TLS). A card that mounts gets its folders
// and loses its stale temps.
void mountNow(const char* why) {
    const size_t heapBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t t0 = millis();
    const Mount m = gCard.mount(static_cast<uint8_t>(gPins.cs), SPI);
    const uint32_t mountMs = millis() - t0;
    if (m != Mount::Mounted) {
        gState = m == Mount::NotFat ? CardState::Unreadable : CardState::None;
        Serial.printf("[sd] %s: %s (%u ms)\n", why,
                      m == Mount::NotFat ? "a card with no FAT16/FAT32 volume (exFAT?)" : "no card",
                      (unsigned)mountMs);
        return;
    }
    gState = CardState::Mounted;
    gCard.mkdir(kCardAppDir);
    gCard.mkdir(kCardDecksDir);
    removeTemps(DeckPlace::Card);
    const uint32_t t1 = millis();
    uint64_t total = 0, freeB = 0;
    uint8_t fat = 0;
    gCard.space(&total, &freeB, &fat);
    char t[16], f[16];
    Serial.printf("[sd] %s: mounted in %u ms, %s %s, %s GB, %s free (%u ms); internal heap %u "
                  "free (%u before), %u largest\n",
                  why, (unsigned)mountMs, cardTypeName(gCard.cardType()), fatName(fat),
                  formatGb(total, '.', t, sizeof t), formatGb(freeB, '.', f, sizeof f),
                  (unsigned)(millis() - t1), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heapBefore,
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

// A deck path's filesystem is usable: always LittleFS, the card while mounted.
bool usable(const String& path) { return !onCard(path) || gState == CardState::Mounted; }

}  // namespace

void begin() {
    if (!tgc::board::sdPins(&gPins)) {
        gState = CardState::NoSlot;
        return;
    }
    SPI.begin(gPins.sck, gPins.miso, gPins.mosi, gPins.cs);
    mountNow("boot");
}

bool remount() {
    if (gState == CardState::NoSlot || gState == CardState::Mounted) return false;
    const CardState before = gState;
    mountNow("mount again");
    if (gState == CardState::None && before == CardState::Removed) gState = CardState::Removed;
    return gState == CardState::Mounted;
}

CardState readFailed() {
    if (gState != CardState::Mounted) return gState;
    gCard.end();
    mountNow("after a read error");
    if (gState == CardState::None) gState = CardState::Removed;
    return gState;
}

void giveUp() {
    if (gState == CardState::NoSlot) return;
    gCard.end();
    gState = CardState::Unreadable;
    Serial.printf("[sd] the card keeps failing reads: unmounted\n");
}

bool cardAnswers() {
    if (gState != CardState::Mounted) return false;
    // FatFs asks the card for its status before it reads the folder.
    DIR* d = opendir(decksFolder(DeckPlace::Card).c_str());
    if (!d) return false;
    closedir(d);
    return true;
}

CardState cardState() { return gState; }
bool hasSlot() { return gState != CardState::NoSlot; }

uint64_t cardTotalBytes() {
    uint64_t total = 0, freeB = 0;
    return gState == CardState::Mounted && gCard.space(&total, &freeB) ? total : 0;
}

uint64_t cardFreeBytes() {
    uint64_t total = 0, freeB = 0;
    return gState == CardState::Mounted && gCard.space(&total, &freeB) ? freeB : 0;
}

// --- deck paths --------------------------------------------------------------

bool onCard(const String& path) { return placeOf(path.c_str()) == DeckPlace::Card; }

fs::FS& fsFor(const String& path) {
    if (onCard(path)) return gCard;
    return LittleFS;
}

String fsPath(const String& path) {
    return onCard(path) ? path.substring(std::strlen(kCardMount)) : path;
}

File openFile(const String& path, const char* mode) {
    if (!usable(path)) return File();
    File f = fsFor(path).open(fsPath(path), mode);
    // The core gives a FAT file a 4 KB stdio buffer (FAT's stat() has no block
    // size). A deck is read in small records at random offsets, so on the card
    // a read keeps one sector: 3.5 KB less heap for the loaded deck, and no
    // 4 KB read behind every glyph. Writes keep 4 KB (multi-sector writes).
    if (f && onCard(path) && mode[0] == 'r') f.setBufferSize(kReadBuffer);
    return f;
}

long fileSize(const String& path) {
    return usable(path) ? fsFileSize(fsFor(path), fsPath(path)) : -1;
}

bool removeFile(const String& path) { return usable(path) && fsFor(path).remove(fsPath(path)); }

bool removeIfExists(const String& path) { return exists(path) && removeFile(path); }

bool renameFile(const String& from, const String& to) {
    if (onCard(from) != onCard(to) || !usable(from)) return false;
    return fsFor(from).rename(fsPath(from), fsPath(to));
}

bool sha256(const String& path, uint8_t out[32]) {
    return usable(path) && fsFileSha256(fsFor(path), fsPath(path), out);
}

bool copyFile(const String& from, const String& to, uint8_t digest[32], CopyProgressFn progress,
              void* ctx) {
    File in = openFile(from, "r");
    if (!in || in.isDirectory()) return false;
    File out = openFile(to, "w");
    if (!out) return false;
    uint8_t* buf = static_cast<uint8_t*>(std::malloc(kCopyChunk));
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    bool ok = buf && (!digest || mbedtls_sha256_starts_ret(&sha, 0) == 0);
    const size_t expected = in.size();
    size_t done = 0;
    while (ok && done < expected) {
        const int n = in.read(buf, kCopyChunk);
        if (n <= 0) {
            ok = false;
            break;
        }
        if (digest) ok = mbedtls_sha256_update_ret(&sha, buf, static_cast<size_t>(n)) == 0;
        if (ok) ok = out.write(buf, static_cast<size_t>(n)) == static_cast<size_t>(n);
        done += static_cast<size_t>(n);
        if (progress) progress(done, ctx);
    }
    if (ok && digest) ok = mbedtls_sha256_finish_ret(&sha, digest) == 0;
    mbedtls_sha256_free(&sha);
    std::free(buf);
    in.close();
    out.flush();
    out.close();
    return ok && done == expected;
}

DeckPlace writePlace() { return gState == CardState::Mounted ? DeckPlace::Card : DeckPlace::Device; }

String deckPathFor(DeckPlace place, const String& file) {
    return String(deckPath(place, file.c_str()).c_str());
}

String decksDir(DeckPlace place) { return String(decksFolder(place).c_str()); }

bool makeDecksDir(DeckPlace place) {
    if (place == DeckPlace::Device) return LittleFS.mkdir(kDeviceDecksDir);
    if (gState != CardState::Mounted) return false;
    gCard.mkdir(kCardAppDir);
    return gCard.mkdir(kCardDecksDir);
}

size_t freeBytes(DeckPlace place) {
    uint64_t freeB = 0;
    if (place == DeckPlace::Card) {
        freeB = cardFreeBytes();
    } else {
        const size_t total = LittleFS.totalBytes();
        const size_t used = LittleFS.usedBytes();
        freeB = total > used ? total - used : 0;
    }
    return freeB > 0xFFFFFFFFull ? static_cast<size_t>(0xFFFFFFFFu) : static_cast<size_t>(freeB);
}

void removeTemps(DeckPlace place) {
    for (const char* name : {kFetchTmpName, kUploadTmpName, kMoveTmpName}) {
        const String tmp = deckPathFor(place, name);
        if (removeIfExists(tmp)) Serial.printf("fs: removed stale %s\n", tmp.c_str());
    }
    if (place == DeckPlace::Card && gState == CardState::Mounted &&
        fsRemoveIfExists(gCard, kCardProgressTmp))
        Serial.printf("fs: removed stale %s%s\n", kCardMount, kCardProgressTmp);
}

String installedPath(const String& file) {
    const String device = deckPathFor(DeckPlace::Device, file);
    if (exists(device)) return device;
    const String card = deckPathFor(DeckPlace::Card, file);
    return exists(card) ? card : String();
}

void listFiles(DeckPlace place, std::vector<DeckFile>& out) {
    out.clear();
    if (place == DeckPlace::Card && gState != CardState::Mounted) return;
    // The folder's VFS path: the card's deck paths are VFS paths already.
    const std::string dir = place == DeckPlace::Card
                                ? decksFolder(place)
                                : std::string(kLittleFsMount) + kDeviceDecksDir;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    for (struct dirent* e = readdir(d); e; e = readdir(d)) {
        const std::string name = e->d_name;
        if (!isDeckFileName(name)) continue;
        struct stat st;
        const std::string full = dir + "/" + name;
        if (::stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        DeckFile f;
        f.name = name;
        f.path = deckPath(place, name);
        f.bytes = static_cast<uint32_t>(st.st_size);
        f.place = place;
        out.push_back(f);
    }
    closedir(d);
    std::sort(out.begin(), out.end(),
              [](const DeckFile& a, const DeckFile& b) { return a.name < b.name; });
}

void listDecks(std::vector<String>& out) {
    out.clear();
    std::vector<DeckFile> device, card;
    listFiles(DeckPlace::Device, device);
    listFiles(DeckPlace::Card, card);
    for (const DeckFile& f : mergeDecks(device, card)) out.push_back(f.path.c_str());
}

void listAllDecks(std::vector<String>& out) {
    out.clear();
    std::vector<DeckFile> files;
    for (DeckPlace place : {DeckPlace::Device, DeckPlace::Card}) {
        listFiles(place, files);
        for (const DeckFile& f : files) out.push_back(f.path.c_str());
    }
}

bool removeOtherCopy(const String& path) {
    const DeckPlace other = onCard(path) ? DeckPlace::Device : DeckPlace::Card;
    const String copy = deckPathFor(other, fileNameOf(path.c_str()).c_str());
    if (!removeIfExists(copy)) return false;
    Serial.printf("fs: removed %s, replaced by %s\n", copy.c_str(), path.c_str());
    return true;
}

bool writeProgressCopy() {
    if (gState != CardState::Mounted) return false;
    // The file the portal's GET /progress serves: srs.bin, else the backup a
    // power cut in a save left.
    const char* state = FsSrsFile::kPath;
    if (!fsExists(state)) state = FsSrsFile::kBak;
    if (!fsExists(state)) return false;
    const uint32_t t0 = millis();
    gCard.mkdir(kCardAppDir);
    const String tmp = String(kCardMount) + kCardProgressTmp;
    const String copy = String(kCardMount) + kCardProgressCopy;
    bool ok = copyFile(state, tmp);
    if (ok) {
        removeIfExists(copy);  // FAT renames do not overwrite
        ok = renameFile(tmp, copy);
    }
    if (!ok) removeIfExists(tmp);
    Serial.printf("[sd] progress copy %s: %s (%ld bytes, %u ms)\n", copy.c_str(),
                  ok ? "written" : "failed", ok ? fileSize(copy) : -1L,
                  (unsigned)(millis() - t0));
    return ok;
}

}  // namespace storage
}  // namespace tgc
