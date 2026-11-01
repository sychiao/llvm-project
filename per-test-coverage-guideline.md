# Per-test coverage：降低 RSS 與提高平行度的實作指南

本研究的目標是：以一個 llvm-lit test 為 coverage 單位，回答「某個 Git 版本的
`xxx.cpp:320` 被哪些測試覆蓋」，並讓數千個測試的收集與匯入成本落在可操作範圍。
優先順序是縮小 instrumentation 範圍、避開巨型 generated table、使用 streaming
export，再限制整條 pipeline 的 concurrency。只有 Python 增量解析，無法解決 native
exporter 已經發生的記憶體峰值或 mapping 解碼成本。

本文記錄截至 2026-11-01 的 container 實驗。MiB/GiB 使用 1024 倍率；RSS 是程序
常駐記憶體峰值，不是 JSON 大小。時間若未另述，僅指一次 native export，包含 mapping
載入、summary、序列化與寫出，不包含測試、profdata merge、Python parsing、資料庫或建置。

## 1. 目前有哪些已完成、哪些仍是實驗

| 技術 | 狀態 | 保留範圍內資料 | 需要什麼變更 |
|---|---|---|---|
| `-format=json-stream` | 已推到本 fork 的 `llvm-cov-stream-json-21` 分支 | 是，沿用 JSON schema | 建置此分支 exporter；原本 JSON 格式保留 |
| `-skip-expansions`，保留 functions | 現有選項，已驗證 macro 關係可衍生 | 保留 function regions；省略 files expansions 欄位 | export 參數 |
| `-fprofile-list` 排除巨型函式／縮小範圍 | 已有完整 llc 實測 | 範圍外不再有可用 coverage | 重編 instrumented producer，重新收集 profile |
| 嚴格 `source:...=allow` + 全域 source forbid | Clang 18 小型測試與 LLVM 單檔實測通過 | 排除未選定義檔 | profile-list；尚無完整 llc RSS benchmark |
| Decoder 穩定後停止、空 branch summary 快路徑 | 私有 LLVM/exporter 實驗通過 | 是，實測 JSON byte-identical | LLVM coverage library 與 llvm-cov 修改 |
| Region vectors exact reserve | 私有 LLVM library 實驗通過 | 是 | LLVM coverage library 修改 |
| Scoped covfun buffer borrow | 私有 LLVM library 實驗通過 | 是 | LLVM coverage library 修改，需保留 ownership 契約 |
| 共用靜態 region 索引、per-test 只寫 hits | 資料模型方向 | 取決於實作 | 尚未完成跨測試共享 loader benchmark |

此文件的 commit **只加入指南**；沒有把上述私有 LLVM library patches 一併套用。
不要假設從此分支直接 build 就包含所有優化。

## 2. 為什麼一個 test 也可能需要數十分鐘

使用完整 llc mapping 的成本，不只取決於本次測試執行了多少函式。Loader 仍會解碼
大量靜態 coverage；execution count=0 的函式也可能消耗大量時間。

一次 RISCV `alu32.ll` 的獨立診斷 export 花了 2,425.60 秒：

| 階段 | Wall 秒 | 占比 |
|---|---:|---:|
| Mapping/profile 載入、解碼與配對 | 1,700.123 | 70.09% |
| File/function summaries | 724.398 | 29.86% |
| File 詳細資料計算與 JSON 寫出 | 0.488 | 0.02% |
| Function JSON 寫出 | 0.474 | 0.02% |

當時 `SelectCode()` 有 331,898 個 regions、126,827 個 virtual file IDs；
`getMatchTable()` 有 807,166 個 regions、342,068 個 virtual file IDs。
後者 execution count=0、branch rows=0，卻仍在 expansion branch summary 花了約 724 秒。
Virtual file ID 是 function-local 的展開識別，不等於不同實體檔案數。

Hotspot 是 decoder 固定反覆傳遞 expansion counters，以及 summary 重複取得 expansion
coverage。`-skip-expansions` 主要控制輸出欄位，不會自動略過這些內部計算。
因此「stdout 還沒有資料」不能直接解讀成 JSON serializer 慢；換 protobuf 也不能
直接消除前面的 mapping/summary 成本。

## 3. 先用 profile-list 控制收集範圍

### 3.1 只收 RISCV CPP 定義，排除 generated/header 函式

```text
[clang]
source:*/llvm/lib/Target/RISCV/*.cpp=allow
source:*=forbid
default:forbid
```

Clang 18 實測支援這種 `source:pattern=allow/forbid` 語法；同一 source 類別先檢查
allow，再檢查 forbid。明確的 `source:*=forbid` 讓其他定義檔直接拒絕，避免舊式
只列 `src:allowed.cpp` 時，header 位置沒有匹配後又退回 main TU 而獲准的情況。

`#include` 不會抹去來源檔位置。目前 RISCV 的 `SelectCode()` 整個函式定義在
`RISCVGenDAGISel.inc`，`getMatchTable()` 定義在 `RISCVGenGlobalISel.inc`；
所以依此規則，它們的定義檔不在允許的 CPP 範圍內。
這是依原始碼位置與小型 Clang 實測得到的預期，尚未以此嚴格規則重編完整 llc
驗證這兩個真實函式的 counters 與整體 RSS。

邊界如下：

- 若函式定義在 CPP，只在函式內 include table 資料，整個函式仍被允許。
- Header 巨集在 CPP 呼叫並產生函式時，Clang 依 expansion location 判定，可能保留。
- 宣告在 header、實際定義在 CPP 的函式，依定義位置選擇；header template 依 header 定義選擇。
- 這是函式層級選擇；被允許函式內的 header/macro regions 不會再逐 region 過濾。
- 無一般來源位置的 compiler-generated 函式仍受 Clang fallback 規則影響。
- `-fprofile-list` 不保證移除所有未使用函式的空 coverage mapping；沒有 counters
  不代表 binary 完全沒有殘留 metadata。

精確絕對路徑比 wildcard 更不易誤匹配。Pattern 是與 Clang SourceManager 記錄的路徑
匹配，不會相對於 profile-list 目錄解析；相對 pattern 必須匹配編譯器實際記錄的拼法。
上例 wildcard 可能匹配多個 checkout，不能當成來源身分驗證。

### 3.2 從 Git diff 選檔

最近十個 commit 的示範使用 `HEAD~10 → HEAD` 淨差異，選出 14 個 C-family 定義檔。
每個選中檔案產生一條精確 `source:/absolute/path=allow`，最後加 source/default forbid。

使用同樣規則重編實際 LLVM `SelectOptimize.cpp`，原本 `-O2` 參數保持一致：

| 項目 | 原本 inst-all | 嚴格 source policy |
|---|---:|---:|
| `__profc_` counter globals | 2,181 | 46 |
| Object bytes | 7,455,616 | 5,300,664 |

單檔編譯約 4.79 秒。這不是完整 llc RSS 測試，也不是 counter slot 數的比較。
Clang integration 在 `-O0`/`-O2` 驗證 CPP、header、inc、template 與 out-of-line 定義；
Git 選檔、空 diff deny-all、blob identity 等共 5 個測試通過。

產生器應保存 base/head commits、選中路徑、目前實際編譯內容的 Git blob OID、policy
hash 與來源 hash。使用帶 policy hash 的檔名放進 compiler flags，避免固定檔名內容
更新卻沒有觸發 Ninja 重編。修改 policy 後，所有受影響的 instrumented objects 都要重編，
並使用新的 matching profiles。

Git diff 選路徑，不會自動 checkout 該歷史版本。`.td` 修改需額外解析到生成 `.inc`
的 dependency，不能只因為 `.td` 不在 C-family 清單就視為無影響。

### 3.3 需要較廣 coverage 時，精確排除巨型函式

若需要 shared backend、IR、Support 等 coverage，可保留 inst-all，但排除已知三個巨型函式：

```text
[clang]
!fun:_ZNK12_GLOBAL__N_124RISCVInstructionSelector13getMatchTableEv
!fun:_ZN4llvm17RISCVDAGToDAGISel10SelectCodeEPNS_6SDNodeE
!fun:_ZN12_GLOBAL__N_115X86DAGToDAGISel10SelectCodeEPN4llvm6SDNodeE
```

此方案保留 X86 GlobalISel `getMatchTable()`。Mangled names 是本版本的實測規則，
升級後要重新確認。被排除函式屬於「未收集」，不是「已知未命中」。

## 4. Streaming export 與增量 parsing 各自解決什麼

```sh
llvm-cov export -format=json-stream -skip-expansions ./llc \
  -instr-profile=test.profdata > test.coverage.json
```

本 fork 的 `json-stream` 使用 `json::OStream`，逐項輸出，不累積完整 root JSON tree。
CoverageMapping 仍會先載入，並非從 binary 到 JSON 的全流程 constant-memory。
原有 `-format=text`/預設 JSON 行為保留；`json-stream` 是新增選項。

同一 RISCV CPP 舊白名單＋排除兩函式、同一 test/profile、同一 exporter，各三次：

| 格式 | Export 秒中位數 | Peak RSS MiB 中位數 | JSON MiB |
|---|---:|---:|---:|
| 原本 default JSON | 2.29 | 1,084.66 | 39.05 |
| json-stream | 1.71 | 271.36 | 39.05 |

六份 JSON byte-identical；RSS 約下降 75%。這是完整 native process 的比較。
更早的未排 table、完整 llc 實驗，約 386 MiB JSON 的 RSS 從 7.51 GiB 降到 1.15 GiB，
但 export 仍約 45 分鐘。Streaming 解決 JSON tree 的記憶體，沒有自動修復慢演算法。
該早期實驗有 19 筆 hash mismatch，不能當成全部 coverage 正確的證明。

Python 端可透過 pipe 用 ijson event parser，逐批轉 pickle 或匯入資料庫，避免 JSON
落地及 `json.load()` 全量常駐。不要一次 `list(iterator)`、保留所有 dataclasses、
使用跨整份資料持續成長的 pickle memo，或設無上限的待寫 queue。
若使用 `ijson.items` 建立完整單筆 function/file，記憶體仍至少取決於最大一筆；
需要嚴格批次上限時，使用 events 與 bounded batches。

Producer 與 consumer 同時存活，pipeline 記憶體需合併計算；consumer 變慢會透過 pipe
backpressure 阻塞 exporter。兩邊 exit status、stderr、JSON 完整 EOF 與資料庫 transaction
都確認後，才能把 coverage 標記為完成。Parsing 不能修復 hash mismatch。

## 5. 保留 macro 關係，避免展開 payload 爆炸

保留 functions、使用 `-skip-expansions` 時，`functions[].regions` 仍包含：

```text
[start_line, start_col, end_line, end_col, count, FileID, ExpandedFileID, kind]
```

`kind == 1` 的 ExpansionRegion 可與同函式 `filenames[]`、其他 regions 建立 graph。
File IDs 必須以 function 為範圍；不可只用實體路徑合併不同 expansion instances。
這能保存 coverage 所知的呼叫位置、展開關係、區域和 counts，不含 macro 名稱、
完整展開 token 或完整 preprocessor trace。

inst-all 排三函式的實測有 126,761 條 expansion edges；不重複附帶 target regions 的
JSONL graph 約 58.51 MiB。另在排四函式的參考組態，衍生出的 38,847 筆 root 關係
與實際輸出的 expansions source-region/filenames multiset 完全一致。

原 expansions schema 會對每個 root 重複整個 function 的 regions/filenames。
排三函式組態估計僅重複 payload 就約 209 GiB，這是推估，沒有實際 dump。
所以應儲存共享 function regions + edges，按需求重建 macro 視圖。
不要使用 `-skip-functions` 後又期待從 functions regions 還原這些關係。

## 6. LLVM 內部優化：時間與 RSS 要分開看

同一 inst-all 排三函式、alu32.ll matching profile、json-stream + skip-expansions，
各三次、逐一執行，對照結果：

| 版本 | Export 秒中位數 | Peak RSS MiB 中位數 |
|---|---:|---:|
| 相同編譯方式 control | 16.61 | 931.55 |
| Counter propagation 穩定即停止＋空 branch 快路徑 | 4.99 | 931.52 |
| 上述＋region vectors exact reserve | 4.68 | 858.56 |

省計算約有 3.3 倍加速，但沒有可辨識 RSS 收益。Exact reserve 再減少約 73 MiB。
這兩項修改分別需要 coverage decoder/library 與 llvm-cov summary caller 的調整，
不能只靠 Python 或 export 選項取得。沒有改變 `getCoverageForExpansion()` 一般 API 語意。
Regression 為 75 PASS、3 UNSUPPORTED；正式與診斷輸出均與對照 JSON byte-identical。

後續同範圍、同樣優化工具實驗，僅在 loader 能保證生命週期的情況借用單一對齊 covfun
section，避免複製 buffer，RSS 由 858.55 → 797.09 MiB，少 61.46 MiB；時間
4.78 → 4.72 秒，差異不足以宣稱穩定加速。原 public reader create 的複製／ownership
契約保留，多 section/未對齊仍複製；也做了原 input 釋放後 public reader 可繼續讀取的測試。

### 剩餘 RSS 主因

Peak 出現在完整 decoded function coverage 與 binary reader 同時存活，早於 JSON 寫出。
inst-all 排三函式的資料容量診斷包含：decoded regions 約 199.40 MiB、branch regions
38.98 MiB、FunctionRecord array 68 MiB、function name heap 40.19 MiB、filename
slots/string heap 約 55.16 MiB、reader covfun copy 約 61.50 MiB。
這些是容量或資料長度，不是可直接相加的精確 RSS 分攤。

ExpansionRegion 元素 payload 約 9.67 MiB；連同 expanded-file regions 的聯集約
32.01 MiB。這不是全部 macro 記憶體，也不是可節省上限，但顯示只省 expansion
計算不會移除其他大量函式、名稱、regions 與 reader metadata。

## 7. 縮小 scope 是持續降低 RSS 的主要手段

同一優化工具（counter/summary 修正＋exact reserve，尚未 borrow），各三次：

| Instrumentation scope | Export 秒中位數 | Peak RSS MiB 中位數 | Regions |
|---|---:|---:|---:|
| inst-all，排三函式 | 4.78 | 858.55 | 2,613,516 |
| inst-all，再排 X86 getMatchTable（參考） | 4.73 | 841.64 | 2,527,494 |
| RISCV 63 CPP 舊白名單，排兩函式 | 1.64 | 267.55 | 237,317 |

inst-all → RISCV 約少 68.8% RSS；再多排一個大 table 只少 16.92 MiB。
注意 RISCV 63 CPP 是較早的 `src:` 策略，仍可能 instrument header/template，
不是第 3 節的新嚴格 source policy。不能把 267.55 MiB 直接當成新規則的實測值。

各組使用自己的 llc 與 matching profile，JSON 範圍不同。範圍縮小代表 coverage
資訊被取捨，不是無損壓縮。只縮小 profdata 或只跑一個 test，也不代表 loader 只建立
命中的函式。此 inst-all profile 只有 30,283/349,996 個函式有 positive region，
仍載入完整 mapping。

改讀 RISCV `.a` 可能排除其他元件，但若 instrumentation 已集中 RISCV，主要 mapping
本來就來自那些 members，額外 RSS 降幅可能很小。Archive 篩選不是 lazy loader，
也不保證省去巨型 table 的解碼。需確認成員選擇、計數匹配及是否遺失 linked binary
應包含的 coverage，不能只依 ELF/`.a` 檔案大小推算 RSS。

## 8. 如何把 RSS 收益轉成安全的 parallel 程度

一個 worker 若同時執行 exporter 與 parser，規劃時應使用：

```text
worker_budget ≈ exporter_peak + parser_peak + batch/IPC overhead
memory_workers = floor((available_memory - fixed_reserve) / worker_budget)
workers = min(memory_workers, CPU_budget, IO_budget, database_budget)
```

上式用分開量測的 peaks 相加可作保守預算；實際峰值未必同時發生，正式值應在目標
cgroup/container、實際並行度下量測。多程序 peak RSS 相加也不等同實體總占用，
因為 shared/file-backed pages 可能重複計數；以 cgroup memory.current/peak 核對總量。
`available_memory` 要受 container limit 約束，不能只看 host RAM。

以下只示範 **4 GiB exporter 記憶體預算、不含 parser/merge/OS** 的算術上限：

| 單個 exporter RSS | 4 GiB 中可容納的數量（向下取整） |
|---|---:|
| 原 JSON，1,084.66 MiB | 3 |
| 同組 json-stream，271.36 MiB | 15 |
| 優化後 inst-all 排三，858.55 MiB | 4 |
| 優化後 RISCV 舊 scope，267.55 MiB | 15 |

這不是建議直接開 15 個 workers，也不是平行吞吐實測。先測 1/2/4，再增加；
記錄 batch wall time、tests/s、cgroup peak、CPU time、IO wait、merge/export/parse
各階段時間與 failures。不要同時使用無限制 lit jobs、export workers、每個 exporter
自動 threads 和 DB writers，造成 oversubscription。大量外層 workers 時可從
`llvm-cov export -num-threads=1` 開始比較；本研究上面的數字多使用預設 threads，
不能直接假設限制 threads 後完全相同。

以已測 2,205 tests 作純估算，1.64 秒 export/test 相當於單 worker 約 60.3 分鐘；
4.78 秒相當於約 175.7 分鐘。這只包含 export，忽略測試間差異、merge、資料庫與
並行競爭；理想除以 worker 數只是吞吐上限，不能當作整批完成時間承諾。

## 9. 建議的 per-test pipeline 與資料保存

1. **固定 build 身分與 scope**：記錄 Git snapshot、source blobs、compiler/build flags、
   profile-list hash、producer binary identity、exporter version。換 scope 後重新建置與收集。
2. **一個 lit test 對應一個獨立收集空間**：一個 test 可以有多個 RUN、pipeline 和多個
   instrumented processes，不能假設恰好一個 profraw。此版本 lit 的 `--per-test-coverage`
   會按 RUN command 改寫 `LLVM_PROFILE_FILE`，不是自動建立 per-lit database。
   如需 robust 隔離，可在 runner/wrapper 中按完整 test ID 分目錄，使用 `%m/%p` 類型
   的 runtime filename substitution；避免被 lit 參數再次覆蓋，也避免 helper 混入同一路徑。
3. **每 test merge**：把歸屬該 test、已確認完成的 raw profiles 合併成 matching profdata。
   `llvm-profdata merge -sparse` 可作起點；不同 binary 的 profiles 不可不檢查就混用。
4. **有界 export queue**：job 保存 test ID、build identity、profdata 與物件集合；限制
   native exporters 數量，並另外量測 merge workers 的 memory。
5. **Streaming consumer**：逐批 parse、轉成資料庫 keys/hits，或保存 event-batch pickle；
   writer queue 有上限。SQLite 可先使用單一 writer 加批次 transaction，解析 workers
   不要各自長時間持有 write lock。是否能增加 DB writers 需以 throughput 量測決定。
6. **獨立 test-result**：保存 PASS/FAIL/UNSUPPORTED、log、duration；coverage 保存
   complete/partial/failed、stderr、mismatch 等狀態。工具 exit=0 不代表沒有 coverage 警告。
7. **可追溯查詢**：region 用 source blob + line/column range + region kind 等穩定資訊
   建共享索引；相同 blob 的 region 描述可重用，但還要納入 compiler/mapping/scope schema
   身分，不能假設所有 build 的 region 切分相同。test identity 亦記錄測試 blob/snapshot。
   每 test 儲存命中 region IDs 的 bitmap/稀疏集合，counts 若有需要另存。
8. **一致性檢查後發布**：完整 EOF、exporter/parser exit status、transaction 成功與
   profile/mapping 一致性檢查後才發布可查詢結果。未 instrument、未收集、已收集但未命中
   三者要區分；查詢目前 working tree 時先確認 blob 一致，或明確使用歷史 snapshot。

同一個 ELF 的靜態 mapping 可在資料模型中共享，但目前「每 test 呼叫一次 llvm-cov」
仍會每次重新載入。真正共用解碼 mapping 的長期 worker/API 是後續方向，尚未由本次
結果驗證其成本與正確性。不要把資料庫去重誤認為 exporter 已共享記憶體。

## 10. 重現與證據定位

本次基準 LLVM source 是 release/21.x、LLVM 21.1.8，commit
`2078da43e25a4623cab2d0d60decddf709aaea28`；producer 使用 Clang 18.1.3，
RelWithDebInfo/O2、assertions、X86/RISCV，clang/lld projects，zlib/zstd/xml disabled。
「inst-all」是 llc 所連結元件的 instrumentation，不是同時 export 所有 clang/lld 工具。

已推送的 streaming 格式實作：

- [新增 opt-in json-stream](https://github.com/sychiao/llvm-project/commit/fe6653f15e60731fc1a2e8f5c5ece68947ef284f)
- [目前分支的 exporter](llvm/tools/llvm-cov/CoverageExporterJson.cpp)
- [Clang profile-list matching](clang/lib/Basic/ProfileList.cpp)
- [Coverage mapping reader](llvm/lib/ProfileData/Coverage/CoverageMappingReader.cpp)
- [Coverage summary](llvm/tools/llvm-cov/CoverageSummaryInfo.cpp)

研究 container 的原始報告位於 `/root/llvm-cov.py/experiments/`，以下是本機證據位置，
不是此 LLVM repository 內已發布的檔案：

| 實驗目錄 | 內容 |
|---|---|
| `streaming-export/REPORT.md` | 完整 llc 原 JSON 與 streaming 的早期大型比較 |
| `export-phase-analysis/REPORT.md` | 2,425 秒的階段 clocks、PC sampling、巨型函式統計 |
| `table-filter/REPORT.md`、`JSON-FORMAT-REPORT.md` | table 排除、default JSON/stream 三次比較 |
| `broad-profile-three/REPORT.md` | inst-all 排三函式、macro graph 與 suite 行為驗證 |
| `internal-expansion/REPORT.md` | decoder/summary/reserve，含 regression 與 byte identity |
| `rss-scope/REPORT.md` | scope 對比、memory inventory、scoped buffer borrow |
| `git-diff-profile/README.md` | Git diff 產生器、14 檔 manifest、Clang/LLVM 單檔驗證 |

私有乾淨 patches：`internal-expansion/lossless-optimizations.patch` 與
`rss-scope/scoped-covfun-borrow.patch`，已通過 apply-check，但不包含於本文件 commit。

完整 CodeGen/RISCV 行為驗證曾為 2,205 PASS，另有兩個 X86 tests PASS；這不表示已
對全部 2,205 tests 逐一 merge/export/import。多數後續效能比較使用同一個 `alu32.ll`
及各 producer 自己的 profile，序列執行、不與編譯或解析重疊。跨不同表格的結果可能
來自不同控制實驗，應採各自組內比較，不把所有降幅連乘成未量測的最終效能。
