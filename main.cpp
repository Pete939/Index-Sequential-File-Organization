#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>
#include <fmt/core.h>

#define KEY_EMPTY (-1)

#include <cassert>
#include <ctime>
#include <cstdint>

namespace sbd {
    class Record {
    public:
        uint64_t id;
        double mass;
        double specificHeat;
        double deltaTemp;

        Record() : mass(0), specificHeat(0), deltaTemp(0), id(-1) {
        }
        Record(uint64_t id, double mass, double specificHeat, double deltaTemp) : id(id),
            mass(mass), specificHeat(specificHeat),
            deltaTemp(deltaTemp) {
        }
    };
}

struct OverflowPointer {
    int pageIdx = KEY_EMPTY;
    int pagePosition = KEY_EMPTY;
};

typedef struct PrimaryAreaElement {
    sbd::Record record;
    OverflowPointer pointer;
    bool deleted = false;
} OverflowAreaElement;

struct IndexAreaElement {
    uint32_t key;
    int32_t pageNumber;
};

struct DiskAccessStatistic {
    uint32_t currentOperationReads = 0;
    uint32_t currentOperationWrites = 0;
    uint32_t overallWrites = 0;
    uint32_t overallReads = 0;
};

struct OperationStats {
    DiskAccessStatistic deleteStats = {};
    DiskAccessStatistic updateStats = {};
    DiskAccessStatistic insertStats = {};
    DiskAccessStatistic reorganizationStats = {};
    DiskAccessStatistic readStats = {};
};

struct Parameters {
    float alpha;
    float maxOverflowRatio;
};

struct DeletedRecordsCount {
    uint32_t deletedOverflow;
    uint32_t deletedMain;
};

struct ProgramCtx {
    uint32_t reorganizationCount;
    uint32_t recordsInMainArea;
    uint32_t pagesInMainArea;
    uint32_t recordsInOverflowArea;
    uint32_t recordsPerPage;
    bool quit;
    bool fileInput;
    bool automaticReorganization;
    OperationStats stats;
    std::vector<IndexAreaElement> index;
    std::ifstream in;
    std::fstream data;
    std::fstream overflow;
    Parameters params;
    DiskAccessStatistic *activeStat;
    DeletedRecordsCount deleted;
};

struct Page {
    uint32_t id;
    std::vector<PrimaryAreaElement> elements;
};

Page ReadPage(ProgramCtx *ctx, std::fstream *fd, uint32_t id) {
    if (ctx->activeStat) {
        ctx->activeStat->currentOperationReads++;
    }
    Page page;
    page.elements.resize(ctx->recordsPerPage);
    int pageSize = ctx->recordsPerPage * sizeof(PrimaryAreaElement);
    fd->seekg(id * pageSize);
    fd->read(reinterpret_cast<char *>(page.elements.data()), pageSize);
    page.id = id;
    return page;
}

void WritePage(ProgramCtx *ctx, std::fstream *fd, Page &page) {
    if (ctx->activeStat) {
        ctx->activeStat->currentOperationWrites++;
    }
    if (page.elements.size() < ctx->recordsPerPage) {
        page.elements.resize(ctx->recordsPerPage);
    }
    int pageSize = ctx->recordsPerPage * sizeof(PrimaryAreaElement);
    fd->seekp(page.id * pageSize);
    fd->write((char *) page.elements.data(), pageSize);
    fd->flush();
}

int CountPages(std::vector<IndexAreaElement> &index) {
    int count = 0;
    for (auto &[key, pageNumber]: index) {
        if (key != KEY_EMPTY) {
            count++;
        }
    }
    return count;
}

void PrintPage(Page &page) {
    int i = 0;
    for (const auto &el: page.elements) {
        if (el.record.id == KEY_EMPTY) {
            std::cout << "Empty record\n";
        } else {
            std::cout << fmt::format(
                "[{}]id:{},(M:{},S:{},D:{}),ovrflw:({},{})",
                i++,
                el.record.id,
                el.record.mass,
                el.record.specificHeat,
                el.record.deltaTemp,
                el.pointer.pageIdx,
                el.pointer.pagePosition
            );
            if (el.deleted) {
                std::cout << "[deleted]\n";
            }
            else {
                std::cout << "\n";
            }
        }
    }
}

void ShowFile(ProgramCtx *ctx) {
    std::cout << ("Main START:\n");
    int pages = CountPages(ctx->index);
    Page page;
    for (uint32_t i = 0; i < pages; ++i) {
        page = ReadPage(ctx, &ctx->data, i);
        std::cout << fmt::format("PAGE: {}\n", i);
        PrintPage(page);
    }
    if (pages == 0) {
        std::cout << ("EMPTY\n");
    }
    std::cout << ("Main END\n");
    std::cout << ("Overflow START:\n");
    ctx->overflow.seekg(0);
    Page ovr;
    pages = std::ceil((float) ctx->recordsInOverflowArea / ctx->recordsPerPage);
    if (ctx->recordsInOverflowArea > 0) {
        for (uint32_t i = 0; i < pages; ++i) {
            ovr = ReadPage(ctx, &ctx->overflow, i);
            std::cout << fmt::format("PAGE: {}\n", i);
            PrintPage(ovr);
        }
    }
    std::cout << ("Overflow END\n");
    if (!ctx->fileInput) {
        std::cout << "Press any key to continue\n";
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::cin.get();
    }
}

sbd::Record InputRecord(ProgramCtx *ctx) {
    uint32_t key;
    double mass, specificHeat, deltaTemp;
    if (!ctx->fileInput) {
        std::cout << "(Id, Mass, Specific heat, Delta temperature): ";
        std::cin >> key >> mass >> specificHeat >> deltaTemp;
    } else {
        ctx->in >> key >> mass >> specificHeat >> deltaTemp;
    }
    return {key, mass, specificHeat, deltaTemp};
}


int FindPageById(ProgramCtx *ctx, uint32_t id) {
    if (ctx->recordsInMainArea == 0) {
        return 0;
    }
    if (ctx->index[0].key > id) {
        return 0;
    }
    uint64_t lastPageIdx = -1;
    for (int i = 0; i < ctx->index.size(); ++i) {
        const auto &el = ctx->index[i];
        if (el.key == KEY_EMPTY) {
            if (i > 0) {
                return i - 1;
            }
            std::cout << ("There should at least one page in the program\n");
            exit(1);
        }
        lastPageIdx = i;
        if ((i + 1 < ctx->index.size()) && ctx->index[i + 1].key != KEY_EMPTY) {
            if (ctx->index[i + 1].key > id) {
                return i;
            }
        }
    }
    return lastPageIdx;
}

void RearrangePage(Page *page, uint32_t startIdx) {
    if (startIdx == 0) {
        return;
    }
    for (uint32_t i = startIdx; i > 0; --i) {
        if (page->elements[i - 1].record.id > page->elements[i].record.id) {
            uint64_t temp = page->elements[i].record.id;
            page->elements[i].record.id = page->elements[i - 1].record.id;
            page->elements[i - 1].record.id = temp;
        } else {
            break;
        }
    }
}

std::vector<OverflowAreaElement> ReadOverflowChain(ProgramCtx *ctx, OverflowPointer startPointer, int maxChainLength) {
    OverflowAreaElement element;
    OverflowPointer ptr = startPointer;
    OverflowPointer prev;
    std::vector<OverflowAreaElement> chain;
    if (maxChainLength != KEY_EMPTY) {
        chain.reserve(maxChainLength);
    }
    Page page;
    while (true) {
        if (prev.pageIdx != ptr.pageIdx) {
            page = ReadPage(ctx, &ctx->overflow, ptr.pageIdx);
        }
        element = page.elements[ptr.pagePosition];
        chain.push_back(element);
        prev = ptr;
        ptr = element.pointer;
        if (element.pointer.pageIdx == KEY_EMPTY) {
            break;
        }
    }
    return chain;
}


// void VerifyOrganization(ProgramCtx *ctx) {
//     int pages = CountPages(ctx->index);
//     Page page;
//     for (uint32_t i = 0; i < pages; ++i) {
//         page = ReadPage(ctx, &ctx->data, i);
//         for (auto &el : page.elements) {
//             if (el.pointer.pageIdx != KEY_EMPTY) {
//                 std::vector<OverflowAreaElement> chain = ReadOverflowChain(ctx, el.pointer, -1);
//                 int previousKey = 0;
//                 for (auto &chainEl : chain) {
//                     if (previousKey >= chainEl.record.id) {
//                         std::cerr << "Incorrect overflow chain\n";
//                         exit(1);
//                     }
//                     previousKey = chainEl.record.id;
//                 }
//             }
//         }
//     }
// }

void OverflowChainInsert(ProgramCtx *ctx, OverflowPointer startPointer, OverflowPointer assignPointer, int key, Page &newRecordPage, Page &startPage, int startPagePosition) {
    OverflowPointer currentPtr = startPointer;
    OverflowPointer previousPtr = {KEY_EMPTY, KEY_EMPTY};

    Page currentPage;
    currentPage.id = KEY_EMPTY;
    Page previousPage;
    previousPage.id = KEY_EMPTY;

    if (currentPtr.pageIdx != KEY_EMPTY) {
        currentPage = ReadPage(ctx, &ctx->overflow, currentPtr.pageIdx);
        if (currentPage.elements[currentPtr.pagePosition].record.id > key) {
            newRecordPage.elements[assignPointer.pagePosition].pointer = currentPtr;
            WritePage(ctx, &ctx->overflow, newRecordPage);

            startPage.elements[startPagePosition].pointer = assignPointer;
            WritePage(ctx, &ctx->data, startPage);
            return;
        }
    }

    while (currentPtr.pageIdx != KEY_EMPTY) {
        if (currentPage.id != currentPtr.pageIdx) {
            currentPage = ReadPage(ctx, &ctx->overflow, currentPtr.pageIdx);
        }
        OverflowAreaElement &currentElem = currentPage.elements[currentPtr.pagePosition];

        if (currentElem.record.id == key) {
            newRecordPage.elements[assignPointer.pagePosition].record.id = KEY_EMPTY;
            WritePage(ctx, &ctx->overflow, newRecordPage);
            ctx->recordsInOverflowArea--;
            if (currentElem.deleted) {
                currentElem.deleted = false;
            }
            else {
                if (!ctx->fileInput) std::cout << "Element already exists!\n";
            }
            return;
        }

        if (currentElem.record.id > key) {
            break;
        }

        previousPtr = currentPtr;
        previousPage = currentPage;
        currentPtr = currentElem.pointer;
    }

    newRecordPage.elements[assignPointer.pagePosition].pointer = currentPtr;

    if (previousPtr.pageIdx == assignPointer.pageIdx) {
        newRecordPage.elements[previousPtr.pagePosition].pointer = assignPointer;
        WritePage(ctx, &ctx->overflow, newRecordPage);
    } else {
        WritePage(ctx, &ctx->overflow, newRecordPage);
        previousPage.elements[previousPtr.pagePosition].pointer = assignPointer;
        WritePage(ctx, &ctx->overflow, previousPage);
    }
}

void OverflowInsertRecord(ProgramCtx *ctx, const sbd::Record &record, Page &page) {
    OverflowPointer recordOp;
    recordOp.pageIdx = ctx->recordsInOverflowArea / ctx->recordsPerPage;
    recordOp.pagePosition = (ctx->recordsInOverflowArea % ctx->recordsPerPage);
    Page overflowPage = ReadPage(ctx, &ctx->overflow, recordOp.pageIdx);
    for (int i = recordOp.pagePosition; i < ctx->recordsPerPage; ++i) {
        overflowPage.elements[i].record = sbd::Record(KEY_EMPTY, 0, 0, 0);
        overflowPage.elements[i].pointer.pageIdx = KEY_EMPTY;
        overflowPage.elements[i].pointer.pagePosition = KEY_EMPTY;
    }
    overflowPage.elements[recordOp.pagePosition].record = record;

    ctx->recordsInOverflowArea++;
    int lastPageElementIdx = ctx->recordsPerPage - 1;
    PrimaryAreaElement lastPageElement = page.elements[lastPageElementIdx];
    if (lastPageElement.record.id < record.id) {
        if (lastPageElement.pointer.pageIdx != KEY_EMPTY) {
            WritePage(ctx, &ctx->overflow, overflowPage);
            OverflowPointer op = {recordOp.pageIdx, recordOp.pagePosition};
            OverflowChainInsert(ctx, lastPageElement.pointer, op, record.id, overflowPage, page, lastPageElementIdx);
            return;
        }
        page.elements[lastPageElementIdx].pointer.pageIdx = recordOp.pageIdx;
        page.elements[lastPageElementIdx].pointer.pagePosition = recordOp.pagePosition;
        WritePage(ctx, &ctx->data, page);
        WritePage(ctx, &ctx->overflow, overflowPage);
        return;
    }
    if (page.elements[0].record.id > record.id) {
        PrimaryAreaElement mainAreaFirstElement = page.elements[0];
        OverflowAreaElement overflowAreaElement = overflowPage.elements[recordOp.pagePosition];

        overflowAreaElement.pointer.pageIdx = recordOp.pageIdx;
        overflowAreaElement.pointer.pagePosition = recordOp.pagePosition;

        overflowPage.elements[recordOp.pagePosition] = mainAreaFirstElement;
        page.elements[0] = overflowAreaElement;

        ctx->index[page.id].key = overflowPage.elements[recordOp.pagePosition].record.id;

        WritePage(ctx, &ctx->data, page);
        WritePage(ctx, &ctx->overflow, overflowPage);
        return;
    }
    for (int i = 1; i < ctx->recordsPerPage; ++i) {
        if (page.elements[i].record.id > record.id) {
            if (page.elements[i - 1].pointer.pageIdx != KEY_EMPTY) {
                WritePage(ctx, &ctx->overflow, overflowPage);
                OverflowChainInsert(ctx, page.elements[i - 1].pointer, recordOp, record.id, overflowPage, page, i - 1);
                return;
            }
            page.elements[i - 1].pointer.pageIdx = recordOp.pageIdx;
            page.elements[i - 1].pointer.pagePosition = recordOp.pagePosition;
            WritePage(ctx, &ctx->data, page);
            WritePage(ctx, &ctx->overflow, overflowPage);
            return;
        }
    }
}

void InsertRecord(ProgramCtx *ctx, sbd::Record updateRecord={}) {
    sbd::Record record;
    if (updateRecord.id == KEY_EMPTY) {
        record = InputRecord(ctx);
    }
    else {
        record = updateRecord;
    }
    int pageIdx = FindPageById(ctx, record.id);
    if (pageIdx == -1) {
        return;
    }
    Page page = ReadPage(ctx, &ctx->data, pageIdx);
    if (ctx->recordsInMainArea == 0) {
        page.elements[0] = {.record = record,  .pointer = {-1, -1}, .deleted = false};
        assert(page.id == 0);
        WritePage(ctx, &ctx->data, page);
        ctx->index[0].key = record.id;
        ctx->recordsInMainArea++;
        return;
    }
    for (int i = 0; i < ctx->recordsPerPage; ++i) {
        uint64_t id = page.elements[i].record.id;
        if (id == record.id) {
            if (page.elements[i].deleted) {
                page.elements[i].deleted = false;
                page.elements[i].record = record;
                WritePage(ctx, &ctx->data, page);
                return;
            }
            std::cout << "Record with such index already exists!\n";
            std::cout << "Press any key to continue\n";
            if (!ctx->fileInput) {
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                std::cin.get();
            }
            return;
        }
        // ReSharper disable once CppDFAUnreachableCode
        if (page.elements[i].record.id == -1) {
            page.elements[i] = {record, -1};
            if (i > 0 && page.elements[i - 1].record.id > record.id) {
                RearrangePage(&page, i);
            }
            WritePage(ctx, &ctx->data, page);
            ctx->recordsInMainArea++;
            return;
        }
    }
    if (!ctx->automaticReorganization) {
        if (ctx->recordsInOverflowArea % ctx->recordsPerPage == 0 && ctx->recordsInOverflowArea != 0) {
            Page emptyPage;
            emptyPage.elements.resize(ctx->recordsPerPage);
            for (auto &el : emptyPage.elements) { el.record.id = KEY_EMPTY; }
            emptyPage.id = ctx->recordsInOverflowArea / ctx->recordsPerPage;
            WritePage(ctx, &ctx->overflow, emptyPage);
        }
    }
    OverflowInsertRecord(ctx, record, page);
}

void Reorganize(ProgramCtx *ctx) {
    int N = ctx->recordsInMainArea - ctx->deleted.deletedMain;
    int V = ctx->recordsInOverflowArea - ctx->deleted.deletedOverflow;
    int b = ctx->recordsPerPage;
    int bf = b;
    int bi = b;
    float alpha = ctx->params.alpha;

    std::string newPrimaryAreaFilename = "data" + std::to_string(ctx->reorganizationCount) + ".bin";
    std::string newOverflowAreaFilename = "overflow" + std::to_string(ctx->reorganizationCount) + ".bin";
    std::string newIndexFilename = "index" + std::to_string(ctx->reorganizationCount) + ".bin";
    ctx->reorganizationCount++;

    std::fstream newData(newPrimaryAreaFilename, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
    std::fstream newOverflow(newOverflowAreaFilename,
                             std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);

    uint32_t newPrimaryAreaSizeInPages = std::ceil(static_cast<float>(N + V) / (static_cast<float>(bf) * alpha));
    char zero = -1;
    uint32_t pageSize = sizeof(PrimaryAreaElement) * ctx->recordsPerPage;
    newData.seekp(newPrimaryAreaSizeInPages * pageSize);
    newData.write(&zero, 1);

    uint32_t overflowAreaNewSize = std::ceil((float) newPrimaryAreaSizeInPages * ctx->params.maxOverflowRatio);

    uint32_t newIndexSizeInPages = std::ceil((float) newPrimaryAreaSizeInPages / (float) b);
    std::vector<IndexAreaElement> index;
    index.reserve(newIndexSizeInPages * ctx->recordsPerPage);
    newOverflow.seekp(overflowAreaNewSize * pageSize);
    newOverflow.write(&zero, 1);

    Page oldMainPage;
    Page newMainPage;
    newMainPage.id = 0;
    newMainPage.elements.reserve(ctx->recordsPerPage);
    int lastPageIdx = -1;
    int currentPageIdx = 0;
    int recordsInCurrentPage = 0;

    std::vector<PrimaryAreaElement> oldPageRecords;
    oldPageRecords.reserve(ctx->recordsPerPage);
    for (int i = 0; i < ctx->pagesInMainArea; ++i) {
        oldPageRecords.clear();
        oldMainPage = ReadPage(ctx, &ctx->data, i);
        int j = 0;
        for (auto &el: oldMainPage.elements) {
            if (el.record.id != KEY_EMPTY) {
                if (!el.deleted) {
                    oldPageRecords.push_back(el);
                }
                if (el.pointer.pageIdx != KEY_EMPTY) {
                    int maxChainLength = -1;
                    if (j < ctx->recordsPerPage - 1) {
                        maxChainLength = oldMainPage.elements[j + 1].record.id - el.record.id;
                        oldPageRecords.reserve(oldPageRecords.size() + maxChainLength);
                    }
                    std::vector<OverflowAreaElement> chain = ReadOverflowChain(ctx, el.pointer, maxChainLength);
                    oldPageRecords.insert(
                        oldPageRecords.end(),
                        chain.begin(),
                        chain.end()
                    );
                }
                for (auto &el : oldPageRecords) {
                    if (el.deleted) {
                        continue;
                    }
                    el.pointer = {KEY_EMPTY, KEY_EMPTY};
                    newMainPage.elements.push_back(el);
                    recordsInCurrentPage++;
                    if (recordsInCurrentPage >= std::ceil(alpha * (float) bf)) {
                        IndexAreaElement iae{};
                        iae.pageNumber = currentPageIdx;
                        iae.key = newMainPage.elements[0].record.id;
                        index.push_back(iae);

                        newMainPage.id = currentPageIdx;
                        WritePage(ctx, &newData, newMainPage);
                        currentPageIdx++;
                        recordsInCurrentPage = 0;
                        newMainPage.elements.clear();
                    }
                }
                oldPageRecords.clear();
            }
            j++;
        }
    }

    if (!newMainPage.elements.empty()) {
        newMainPage.id = currentPageIdx;
        WritePage(ctx, &newData, newMainPage);
        IndexAreaElement iae{};
        iae.pageNumber = newMainPage.id;
        iae.key = newMainPage.elements[0].record.id;
        index.push_back(iae);
    }
    ctx->overflow.close();
    ctx->data.close();
    ctx->index.clear();

    ctx->overflow = std::move(newOverflow);
    ctx->index = std::move(index);
    ctx->data = std::move(newData);
    ctx->recordsInMainArea = N + V;
    ctx->recordsInOverflowArea = 0;
    ctx->pagesInMainArea = currentPageIdx + 1;
    ctx->deleted.deletedMain = 0;
    ctx->deleted.deletedOverflow = 0;
}

struct MainPageSearchResult {
    bool found;
    int elementIndex;
    OverflowPointer overflow;
};

MainPageSearchResult BinarySearchMainPage(const Page &page, int key) {
    int left = 0;
    int right = page.elements.size() - 1;

    int lastSmallerIdx = -1;

    while (left <= right) {
        int mid = left + (right - left) / 2;
        const auto &el = page.elements[mid];

        if (el.record.id == KEY_EMPTY) {
            right = mid - 1;
            continue;
        }

        if (el.record.id == key) {
            return {
                .found = true,
                .elementIndex = mid,
                .overflow = el.pointer
            };
        }

        if (el.record.id < key) {
            lastSmallerIdx = mid;
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }

    if (lastSmallerIdx != -1) {
        return {
            .found = false,
            .elementIndex = lastSmallerIdx,
            .overflow = page.elements[lastSmallerIdx].pointer
        };
    }

    // key < first element
    return {
        .found = false,
        .elementIndex = -1,
        .overflow = { KEY_EMPTY, KEY_EMPTY }
    };
}

struct FoundRecordCtx {
    bool found;
    bool foundInMain;
    PrimaryAreaElement element;
    OverflowPointer pageInfo; // pageIdx and Position;
};

FoundRecordCtx ReadRecord(ProgramCtx * ctx, bool silent = false) {
    int key;
    if (!ctx->fileInput) {
        std::cout << "Id:";
        std::cin >> key;
    } else {
        ctx->in >> key;
    }
    int pageIdx = FindPageById(ctx, key);
    if (pageIdx == KEY_EMPTY) {
        if (!silent) {
            std::cerr << fmt::format("Could not find an element with id: {}\n", key);
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            std::cin.get();
        }
        return {false};
    }
    Page page = ReadPage(ctx, &ctx->data, pageIdx);

    MainPageSearchResult result = BinarySearchMainPage(page, key);

    if (result.found) {
        if (page.elements[result.elementIndex].deleted) {
            if (!silent) {
                std::cerr << fmt::format("Could not find an element with id: {}\n", key);
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                std::cin.get();
            }
            return {false};
        }
        const auto &el = page.elements[result.elementIndex];
        if (!silent) {
            std::cout << fmt::format(
                "id:{},(M:{},S:{},D:{}),ovrflw:({},{})\n",
                el.record.id,
                el.record.mass,
                el.record.specificHeat,
                el.record.deltaTemp,
                el.pointer.pageIdx,
                el.pointer.pagePosition
            );
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            std::cin.get();
        }
        return {true, true, el, {(int)page.id, result.elementIndex}};
    }

    OverflowPointer startPointer = result.overflow;

    if (startPointer.pageIdx == KEY_EMPTY) {
        if (!silent) {
            std::cerr << fmt::format("Could not find an element with id: {}\n", key);
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            std::cin.get();
        }
        return {false};
    }

    // ReSharper disable once CppDFAUnreachableCode
    OverflowAreaElement element;
    OverflowPointer ptr = startPointer;
    OverflowPointer prev;
    while (true) {
        if (prev.pageIdx != ptr.pageIdx) {
            page = ReadPage(ctx, &ctx->overflow, ptr.pageIdx);
        }
        element = page.elements[ptr.pagePosition];
        if (element.record.id == key && element.deleted == false) {
            if (!silent) {
                std::cout << fmt::format(
                   "id:{},(M:{},S:{},D:{}),ovrflw:({},{})\n",
                   element.record.id,
                   element.record.mass,
                   element.record.specificHeat,
                   element.record.deltaTemp,
                   element.pointer.pageIdx,
                   element.pointer.pagePosition
               );
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                std::cin.get();
            }
            return {true, false, element, {(int)page.id, ptr.pagePosition}};
        }
        prev = ptr;
        ptr = element.pointer;
        if (element.pointer.pageIdx == KEY_EMPTY) {
            break;
        }
    }

    if (!silent) {
        std::cerr << fmt::format("Could not find an element with id: {}\n", key);
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::cin.get();
    }
    return {false};
}

void DeleteRecord(ProgramCtx * ctx) {
    FoundRecordCtx recordCtx = ReadRecord(ctx, true);
    if (recordCtx.found == false || recordCtx.element.deleted) {
        std::cerr << fmt::format("Provided element does not exist\n");
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::cin.get();
        return;
    }
    Page page;
    std::fstream *file;
    if (recordCtx.foundInMain) {
        file = &ctx->data;
        ctx->deleted.deletedMain++;
    }
    else {
        file = &ctx->overflow;
        ctx->deleted.deletedOverflow++;
    }
    page = ReadPage(ctx, file, recordCtx.pageInfo.pageIdx);
    page.elements[recordCtx.pageInfo.pagePosition].deleted = true;
    WritePage(ctx, file, page);
}

void UpdateRecord(ProgramCtx * ctx) {
    FoundRecordCtx recordCtx = ReadRecord(ctx, true);
    if (recordCtx.found == false || recordCtx.element.deleted) {
        std::cerr << fmt::format("Provided element does not exist\n");
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::cin.get();
        return;
    }
    Page page;
    std::fstream *file;
    if (recordCtx.foundInMain) {
        file = &ctx->data;
        ctx->deleted.deletedMain++;
    }
    else {
        file = &ctx->overflow;
        ctx->deleted.deletedOverflow++;
    }
    page = ReadPage(ctx, file, recordCtx.pageInfo.pageIdx);
    page.elements[recordCtx.pageInfo.pagePosition].deleted = true;
    WritePage(ctx, file, page);
    std::cout << "Input updated data for the record:\n";
    int id;
    double mass, deltaTemp, specificHeat;
    if (!ctx->fileInput) {
        std::cout << "(Id, Mass, Specific heat, Delta temperature): ";
        std::cin >> id >> mass >> specificHeat >> deltaTemp;
    } else {
        ctx->in >> id >> mass >> specificHeat >> deltaTemp;
    }
    if (id == recordCtx.element.record.id) {
        page.elements[recordCtx.pageInfo.pagePosition].record.mass = mass;
        page.elements[recordCtx.pageInfo.pagePosition].record.deltaTemp = deltaTemp;
        page.elements[recordCtx.pageInfo.pagePosition].record.specificHeat = specificHeat;
        WritePage(ctx, file, page);
        return;
    }
    InsertRecord(ctx, sbd::Record(id, mass, specificHeat, deltaTemp));
}

void PrintStats(const DiskAccessStatistic& stats) {
    std::cout << fmt::format("Last operations reads: {}\n", stats.currentOperationReads);
    std::cout << fmt::format("Last operation writes: {}\n", stats.currentOperationWrites);
    std::cout << fmt::format("Overall reads: {}\n", stats.overallReads);
    std::cout << fmt::format("Overall writes: {}\n", stats.overallWrites);
}

void ShowStatistics(ProgramCtx * ctx) {
    std::cout << "(I)nsert | (R)ead | (U)pdate | R(e)organize | (D)elete\n";
    std::cout << "Pick a statistic: ";
    char option;
    std::cin >> option;
    option = std::tolower(option);
    switch (option) {
        case 'e':
            PrintStats(ctx->stats.reorganizationStats);
            break;
        case 'i':
            PrintStats(ctx->stats.insertStats);
            break;
        case 'r':
            PrintStats(ctx->stats.readStats);
            break;
        case 'u':
            PrintStats(ctx->stats.updateStats);
            break;
        case 'd':
            PrintStats(ctx->stats.deleteStats);
            break;
        default:
            std::cerr << "Invalid option\n";
            break;
    }
    std::cout << "Press any key to continue\n";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::cin.get();
}

void UpdateStatistics(DiskAccessStatistic &stats) {
    stats.overallReads += stats.currentOperationReads;
    stats.overallWrites += stats.currentOperationWrites;
}

void ClearOperationStatistic(DiskAccessStatistic &stats) {
    stats.currentOperationReads = 0;
    stats.currentOperationWrites = 0;
}

void OperationSummary(ProgramCtx *ctx) {
    uint32_t totalReads = 0;
    uint32_t totalWrites = 0;

    totalReads += ctx->stats.deleteStats.overallReads;
    totalReads += ctx->stats.insertStats.overallReads;
    totalReads += ctx->stats.updateStats.overallReads;
    totalReads += ctx->stats.reorganizationStats.overallReads;
    totalReads += ctx->stats.readStats.overallReads;

    totalWrites += ctx->stats.deleteStats.overallWrites;
    totalWrites += ctx->stats.insertStats.overallWrites;
    totalWrites += ctx->stats.updateStats.overallWrites;
    totalWrites += ctx->stats.reorganizationStats.overallWrites;
    totalWrites += ctx->stats.readStats.overallWrites;

    if (!ctx->fileInput) {
        std::cout << fmt::format("Total reads: {}\nTotal writes: {}\n", totalReads, totalWrites);
        return;
    }
    std::cout << totalReads << " " << totalWrites;
}

bool OverflowRatioExceeded(ProgramCtx *ctx) {
    if (!ctx->automaticReorganization) {
        return false;
    }
    float ratio = static_cast<float>(ctx->recordsInOverflowArea) / static_cast<float>(ctx->recordsInMainArea);
    return ratio >= ctx->params.maxOverflowRatio;
}

void ViewIndex(ProgramCtx *ctx) {
    std::cout << "INDEX START\n";
    for (auto &idx : ctx->index) {
        if (idx.key != KEY_EMPTY) {
            std::cout << fmt::format("Page idx: {}, Key: {}\n", idx.pageNumber, idx.key);
        }
        else {
            std::cout << fmt::format("Page idx: {}, EMPTY\n", idx.pageNumber);
        }
    }
    std::cout << "INDEX END\n";
    std::cout << "Press any key to continue\n";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::cin.get();
}

void ShowOptions(ProgramCtx *ctx) {
    std::cout << ("Choose an option:\n");
    std::cout << ("Insert record(I/i)\n");
    std::cout << ("Read record(R/r)\n");
    std::cout << ("Delete record(D/d)\n");
    std::cout << ("Update record(U/u)\n");
    std::cout << ("View file(F/f)\n");
    std::cout << ("View index(X/x)\n");
    std::cout << ("Reorganize the file(E/e)\n");
    std::cout << ("Show statistics(S/s)\n");
    std::cout << ("Set parameter(P/p)\n");
    std::cout << ("Operation summary(O/o)\n");
    std::cout << ("Quit program (Q/q)\n");
}

void SetParameter(ProgramCtx * ctx) {
    char option;
    if (!ctx->fileInput) {
        std::cout << "Choose a parameter to set value of\n";
        std::cout << "(a)lpha | (o)verflow-ratio | automatic-(r)eorganization";
        std::cin >> option;
    }
    else {
        ctx->in >> option;
    }
    switch (option) {
        case 'a':
            if (ctx->fileInput) {
                ctx->in >> ctx->params.alpha;
            }
            else {
                std::cin >> ctx->params.alpha;
            }
            break;
        case 'o':
            if (ctx->fileInput) {
                ctx->in >> ctx->params.maxOverflowRatio;
            }
            else {
                std::cin >> ctx->params.maxOverflowRatio;
            }
            break;
        case 'r':
            if (ctx->fileInput) {
                ctx->in >> ctx->automaticReorganization;
            }
            else {
                std::cin >> ctx->automaticReorganization;
            }
    }
}

void PerformOption(ProgramCtx *ctx, int option) {
    switch (option) {
        case 'x':
            ViewIndex(ctx);
            break;
        case 'o':
            OperationSummary(ctx);
            break;
        case 'p':
            SetParameter(ctx);
            break;
        case 's':
            ShowStatistics(ctx);
            break;
        case 'u':
            ctx->activeStat = &ctx->stats.updateStats;
            ClearOperationStatistic(ctx->stats.updateStats);
            UpdateRecord(ctx);
            UpdateStatistics(ctx->stats.updateStats);
            break;
        case 'd':
            ctx->activeStat = &ctx->stats.deleteStats;
            ClearOperationStatistic(ctx->stats.deleteStats);
            DeleteRecord(ctx);
            UpdateStatistics(ctx->stats.deleteStats);
            if (OverflowRatioExceeded(ctx)) {
                ClearOperationStatistic(ctx->stats.reorganizationStats);
                ctx->activeStat = &ctx->stats.reorganizationStats;
                Reorganize(ctx);
                UpdateStatistics(ctx->stats.reorganizationStats);
            }
            break;
        case 'r':
            ctx->activeStat = &ctx->stats.readStats;
            ClearOperationStatistic(ctx->stats.readStats);
            ReadRecord(ctx);
            UpdateStatistics(ctx->stats.readStats);
            break;
        case 'i':
            ctx->activeStat = &ctx->stats.insertStats;
            ClearOperationStatistic(ctx->stats.insertStats);
            InsertRecord(ctx);
            UpdateStatistics(ctx->stats.insertStats);
            if (OverflowRatioExceeded(ctx)) {
                ClearOperationStatistic(ctx->stats.reorganizationStats);
                ctx->activeStat = &ctx->stats.reorganizationStats;
                Reorganize(ctx);
                UpdateStatistics(ctx->stats.reorganizationStats);
            }
            break;
        case 'e':
            ctx->activeStat = &ctx->stats.reorganizationStats;
            ClearOperationStatistic(ctx->stats.reorganizationStats);
            Reorganize(ctx);
            UpdateStatistics(ctx->stats.reorganizationStats);
            break;
        case 'f':
            ctx->activeStat = nullptr;
            ShowFile(ctx);
            break;
        case 'q':
            ctx->quit = true;
            break;
        default:
            std::system("clear");
            break;
    }
}

void DeleteOldFiles(ProgramCtx *ctx) {
    for (uint32_t i = 0; i < ctx->reorganizationCount; ++i) {
        std::string oldPrimaryAreaFilename = "data" + std::to_string(i) + ".bin";
        std::string oldOverflowAreaFilename = "overflow" + std::to_string(i) + ".bin";
        std::string oldIndexFilename = "index" + std::to_string(i) + ".bin";
        std::remove(oldIndexFilename.c_str());
        std::remove(oldOverflowAreaFilename.c_str());
        std::remove(oldPrimaryAreaFilename.c_str());
    }
    std::remove("data.bin");
    std::remove("overflow.bin");
    std::remove("index.bin");
}

int main(int argc, char **argv) {
    uint32_t blockingFactor = 4;
    char filename[256] = "test.txt";
    if (argc == 2) {
        blockingFactor = std::strtol(argv[1], nullptr, 10);
    }
    if (argc == 3) {
        strcpy(filename, argv[2]);
    }
    int x = 5;
    std::fstream data("data.bin", std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
    std::fstream overflow("overflow.bin", std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
    std::ifstream in(filename);
    if (!data.good() || !overflow.good() || !in.good()) {
        std::cout << "Failed to open files\n";
        exit(1);
    }

    Page empty;
    empty.elements.resize(blockingFactor);
    for (auto &el: empty.elements) {
        el.record.id = KEY_EMPTY;
        el.deleted = false;
        el.pointer.pageIdx = el.pointer.pagePosition = KEY_EMPTY;
    }
    int pageSize = blockingFactor * sizeof(PrimaryAreaElement);

    data.write((char *) empty.elements.data(), pageSize);
    data.flush();
    overflow.write((char *) empty.elements.data(), pageSize);
    overflow.flush();

    ProgramCtx ctx = {
        .reorganizationCount = 0,
        .recordsInMainArea = 0,
        .pagesInMainArea = 1,
        .recordsInOverflowArea = 0,
        .recordsPerPage = blockingFactor,
        .quit = false,
        .fileInput = true,
        .automaticReorganization = false,
        .stats = {},
        .index = {},
        .in = std::move(in),
        .data = std::move(data),
        .overflow = std::move(overflow),
        .params = {.alpha = 0.5f, .maxOverflowRatio = 0.2f},
        .activeStat = nullptr,
        .deleted = {.deletedOverflow = 0, .deletedMain = 0},
    };

    ctx.index.reserve(ctx.recordsPerPage);
    for (int i = 0; i < ctx.index.capacity(); ++i) {
        IndexAreaElement ie{};
        ie.key = -1;
        ie.pageNumber = i;
        ctx.index.push_back(ie);
    }

    while (!ctx.quit) {
        if (ctx.in.eof()) {
            ctx.fileInput = false;
        }
        char option;
        if (!ctx.fileInput) {
            ShowOptions(&ctx);
            std::string line;
            std::getline(std::cin, line);
            if (line.empty()) {
                option = '\n';
            } else {
                option = std::tolower(line[0]);
            }
        } else {
            ctx.in >> option;
        }
        PerformOption(&ctx, option);
    }
    DeleteOldFiles(&ctx);
    std::cout << ("Closing program\n");
    return 0;
}
