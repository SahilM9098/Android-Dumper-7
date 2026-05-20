# Universal Android Unreal Engine SDK Dumper

> **Credits & Acknowledgements**
> This universal dumper is inspired by and references the incredible work of:
> * **Dumper-7** by [Encryqed](https://github.com/Encryqed/Dumper-7) (The pioneer PC Unreal Engine dumper)
> * **iOS-Dumper-7** by [Aethereux](https://github.com/Aethereux/iOS-Dumper-7) (The arm64 porting foundations for mobile platforms)

---

## 📱 Overview

This project is a high-performance, **Internal-only** Universal Unreal Engine SDK Dumper designed specifically for Android target devices running on `arm64-v8a` architectures. 

It is engineered to run entirely inside the game process address space. By mapping and reading process memory directly, it avoids the massive performance penalties and page-fault issues associated with external memory scanning (via `ptrace` or `/proc/self/mem`). It dynamically locates Unreal Engine's core reflection roots (`GUObjectArray` and `FNamePool`) and programmatically generates ready-to-compile, Dumper-7-style C++ SDK headers containing all classes, structures, enums, and functions of the target game.

> [!WARNING]
> **Custom Engine Limitations**
> This dumper targets **standard, vanilla, or semi-standard** Unreal Engine 4 and Unreal Engine 5 implementations. It **does not** support heavily customized, encrypted, or obfuscated UE versions where structure layouts, padding, class sizes, or vtable hierarchies have been modified or shifted by custom proprietary compilation pipelines.

---

## 📂 SDK Dump Output Location

Once the dump process is successfully initialized and executed via the built-in controller, the generated C++ headers are written directly to the Android device.

* **Default Dump Path:**
  `/sdcard/Android/media/<package_name>/SDK`
  *(e.g., `/sdcard/Android/media/com.game.name/SDK`)*
* **Main Entry Header:** 
  The primary umbrella inclusion header is saved at `/sdcard/Android/media/<package_name>/SDK/SDK.hpp`. Including this file in your external/internal mod projects imports all reconstructed game structures and classes.
* **Customization:**
  The output directory path can be directly edited inside the ImGui control panel under the **Offsets** or **SDK Dumper** interface before starting the dump.

---

## 📦 Reconstructed Assets & Developer Tooling

Every successful dump run generates more than just standard C++ header files. The dumper produces a full suite of highly optimized analysis and static renaming assets designed to accelerate your reverse-engineering workflow:

| Asset File | Format | Description & Usage Heuristics |
| :--- | :--- | :--- |
| **`SDK.hpp`** | C++ Header | The master umbrella include file. Including this single header imports all reconstructed structures, enums, classes, and parameter layouts. |
| **`NamesDump.txt`** | Plain Text | A flat, ordered catalog of all strings loaded in the global `FNamePool` (`GNames`). Invaluable for finding custom assets, mapping namespaces, or simple `Ctrl+F` string reference searching. |
| **`ObjectsDump.txt`** | Plain Text | A live memory index of *every active GObject instance* loaded in the `GUObjectArray` at the second of the dump. Includes instance memory addresses, internal indices, and OuterPrivate-resolved object hierarchy paths. |
| **`Offsets.log`** | Plain Text | Human-readable log detailing the dynamically resolved addresses of core reflection roots (`GUObjectArray`, `FNamePool`, `GWorld`, `ProcessEvent`) and engine internal offsets. |
| **`Functions.log`** | Plain Text | A consolidated registry of all native functions resolved during the dump. Functions are sorted sequentially by their absolute virtual offsets (`nativeFuncPtr - LibBase`) to easily trace executable code neighborhoods. |
| **`Mappings.json`** | JSON | Machine-readable, standardized dictionary mapping all classes, structs, member offsets, and native functions. Perfect for scripting, automation, or custom overlay-menu generators. |
| **`SDKRename_IDA.py`** | Python | A ready-to-run IDA Pro script that automatically remaps stripped, offset-based functions (e.g. `sub_F83C10`) inside `libUE4.so` or `libUnreal.so` to their original, fully qualified Unreal Engine names. |
| **`SDKRename_Ghidra.py`** | Python | An equivalent Ghidra label creation script to instantly rename stripping-affected subroutines inside the Ghidra decompiler database. |

---

## 🛠️ Compilation & Build Methods

The project Gradle configuration exposes **two distinct methods** of compiling the application to optimize developer iteration speeds:

### Method 1: Full Build Pipeline
* **Command:** `./gradlew buildFullPipeline`
* **Workflow:** 
  1. Compiles all Kotlin and Java front-end components.
  2. Builds the APK and extracts `classes.dex`.
  3. Formats the raw bytecode bytes into a C++ hex array and regenerates `app/src/main/cpp/dex_payload.h`.
  4. Triggers CMake/NDK to compile the C++ library `libmenu.so` containing the embedded DEX payload.
  5. Copies the finalized library to the `/output` directory.
* **When to use:** Use this whenever Java, Kotlin, UI components, or payload loaders are modified.

### Method 2: Native-Only Compile (Fast Iteration)
* **Command:** `./gradlew buildNativeOnly`
* **Workflow:**
  1. Skips the Java/Kotlin compile and DEX extraction entirely.
  2. Directly compiles the C++ native sources using the **existing** `dex_payload.h` generated from a previous full run.
  3. Copies the finalized `libmenu.so` library to the `/output` directory.
* **When to use:** Use this during development loops when you are only editing C++ files (`OffsetFinder.cpp`, `SDKDumper.cpp`, engine structures, etc.) to skip lengthy APK assemblies.

---

## 🚀 Deployment & Injection

Because the dumper operates as an **internal library**, it must be injected into the address space of the target game process post-boot.

1. Locate the compiled `libmenu.so` under the `/output` folder.
2. Inject the library into your arm64-v8a game process using:
   * **Zygisk / Magisk Modules**: Hooks the game process at fork time (highly recommended for root environments).
   * **Linker Injectors**: Run command-line injectors (e.g. `inject`) or hook systems to force-load the dynamic library via `dlopen`.
3. **Execution Hook:**
   Upon loading, a constructor attribute `payload_main()` triggers instantly, spawning a separate thread (`inject_thread`) to attach the Java VM, load the embedded Dex payloads from memory via `InMemoryDexClassLoader`, and invoke `com.sahilm9098.arkmodmenu.Loader.main()` to start the internal UI and hooks.

---

## 🔍 Technical Details: Reflection Root Search Strategies

The dumper contains a highly advanced, resilient, and multi-layered search module (`OffsetFinder`) to locate core Unreal Engine structures inside a stripped, release-build `libUE4.so` or `libUnreal.so`. 

The strategies are chained in order of priority and run sequentially until a candidate passes strict structural layout validations.

### 1. GUObjectArray (UObjects) Search Strategies

To find `GUObjectArray` (the global array containing all active reflection objects in the engine), the following strategies are attempted:

| Strategy | Search Type | Detailed Logic & Operations |
| :--- | :--- | :--- |
| **Symbol** | Static & Dynamic Symbol Lookup | Tries resolving known mangled and unmangled symbol names (`GUObjectArray`, `_Z13GUObjectArray`, `_ZN12FUObjectArray13GUObjectArrayE`, `ObjectsArray`) using `FindSymbolDynamic` (for dynamic symbol tables) and `FindSymbolDisk` (reading the raw ELF from the APK file). Candidates are instantly checked via structural layout probing. |
| **String-Ref** | Literal materialization trace | 1. Scans `.rodata` for known string constants (e.g., `"GUObjectArray"`, `"FUObjectArray"`, `"MaxObjectsNotConsideredByGC"`, `"UObjectBase::IsValidLowLevelFast"`).<br>2. Scans `.text` segments for AArch64 instructions that materialize these literals (`ADRP+ADD` or `ADRP+LDR` pairs).<br>3. From the reference site, walks local instructions to trace registers resolving to writable segments (`.data` / `.bss`).<br>4. Structural validation is called on those targets. |
| **Code-Ref Quick** | Fast Instruction Scan | Performs a quick AArch64 instruction scan over high-probability function neighborhoods to locate `ADRP+ADD` or `ADRP+LDR` materializations near common entry points, speeding up detection on standard builds. |
| **Code-Ref** | Exhaustive Instruction Scan | Walks the entire executable `.text` segment instruction-by-instruction. It decodes all AArch64 `ADRP+ADD`/`ADRP+LDR` pairs, tracks dynamic register materializations, and whenever a writeable-segment address target is encountered, validates it with a layout probe. This is extremely robust and **engine-agnostic** since it does not depend on literals surviving compilation. |
| **Pattern** | Byte Signature Match | Scans executable segments for custom or known byte signature patterns to locate the target `GUObjectArray` reference directly. |
| **BSS Scan** | Structural Brute Scan | A fast, exhaustive walk of `libUE4.so`'s writable segments (`.data` and `.bss` PT_LOAD segments) at 8-byte strides. It runs layout validations on every address. Because validation is highly strict, non-UObject targets fail in 1-2 memory reads, making it cheap and safe. |
| **DeepScan** | Cross-Library BSS Scan | Performs a structural BSS scan across **all loaded shared libraries** on the Android device. This is crucial for modern UE5 games where engine modules are split, and `GUObjectArray` may reside in a sibling library (like `libCoreUObject.so` instead of `libUnreal.so`). |

#### 🛡️ GUObjectArray Auto-Detection & Validation (`Probe`)
Every candidate address found by the scanner is run through an advanced, multi-layered layout auto-detection and validation pass (`ObjectArray::Probe`). A candidate is only accepted and initialized if it passes the following structural validation criteria:

1. **Outer Wrapper vs. Inner Address Auto-Detection:**
   The dumper automatically detects if the provided address points to the outer `GUObjectArray` (with the inner `TUObjectArray` at `+0x10` offset) or directly to the inner `TUObjectArray` member.
2. **Chunked vs. Flat Layout Detection:**
   It auto-detects if the array is **Chunked (`FChunkedFixedUObjectArray`)** or **Flat (`FFixedUObjectArray`)** by validating count heuristics:
   * **Chunked:** Validates `NumElements`, `MaxElements`, `NumChunks`, `MaxChunks`, and the `Objects` chunk array pointer.
   * **Flat:** Validates `NumElements`, `MaxElements`, and the flat `Objects` array pointer.
   * **Heuristics:** Enforces that `NumElements` is sane ($1,024 \le N \le 100,000,000$) and `MaxElements \ge NumElements`.
3. **`FUObjectItem` Stride Size Auto-Detection:**
   The dumper automatically determines the stride size of `FUObjectItem` structures (either `0x18` for older UE4.13–UE4.21 versions, or `0x20` for UE4.22+ and UE5). It does this by testing both stride hypotheses on the first few entries and checking if the resolved `Object` pointers point to valid `UObject` instances.
4. **`UObject` Layout Invariant Checking:**
   For a candidate layout, it performs strict structural dereference checks on the first 512 objects:
   * The `Object` pointer is decoded (with optional decryption applied).
   * The object's `InternalIndex` (found at `UObject + 0x0C`) must exactly match its array position index.
   * The object's `vtable` pointer (found at `UObject + 0x00`) must point to a readable, executable memory segment in the game's loaded modules (specifically within `libUE4` / `libUnreal` or mapped segments).
5. **Pointer Decryption Callback:**
   Supports games that encrypt or XOR-mask stored `UObject` pointers in the object array via `ObjectArray::SetDecryption` (passing a custom decryption lambda).

---

### 2. GNames (FNamePool / FNames) Search Strategies

To find `FNamePool` (which stores the strings representing Unreal Engine object and type names), the engine exposes the following strategies:

| Strategy | Search Type | Detailed Logic & Operations |
| :--- | :--- | :--- |
| **Symbol** | Dynamic Symbol Lookup | Tries resolving known symbol patterns (like `GNames`, `FNamePool`, etc.) from dynsym or disk symbol tables, running validations on each candidate. Includes fuzzy substring scan over `.dynsym` table for symbols containing "NamePool", "GNameBlocks", or "GNames". |
| **Func-Walk** | Control Flow Disassembly | 1. Resolves high-probability FName constructor/conversion symbol anchors from the dynamic/disk tables (e.g., `FName::ToString`, `FName::AppendString`, and constructors like `_ZN5FNameC1EPKc9EFindName`).<br>2. Disassembles a window of instructions (`kFuncWalkBytes`) in the target functions.<br>3. Dynamically decodes `ADRP+ADD` instructions and follows branches (`B`/`BL`) one hop deep to trace and extract candidate `FNamePool` materialization addresses inside secondary child functions (e.g., `FNamePool::Find`). |
| **String-Ref** | Literal materialization trace | Scans `.rodata` for strings embedded in FName functions (such as `"Failed to find name '%s'"`, `"Name table corrupted"`, `"Reserve %d entries"`, or `"NamePoolData"`). It traces their code materialization references in `.text` to isolate nearby `FNamePool` global variables. |
| **Code-Ref Quick** | Fast Instruction Scan | A fast instruction scan over high-probability function neighborhoods to identify `ADRP+ADD` or `ADRP+LDR` materializations, speeding up detection. |
| **Code-Ref** | Exhaustive Instruction Scan | Instruction-by-instruction scan of the executable memory space to identify `ADRP+ADD` or `ADRP+LDR` instructions resolving to writeable data segments, running name validation checks on each target. |
| **Pattern** | Byte Signature Match | Scans executable segments for custom or known byte signature patterns. |
| **BSS Scan** | Structural Brute Scan | Walks `.data` and `.bss` segments at 8-byte strides and runs strict verification checks. |

#### 🛡️ GNames Validation (`Probe`) & Layout Detection
`FNamePool` candidates are validated dynamically by checking for a valid `"None"` entry at structural `Blocks[0]`, ensuring that character boundaries and length headers perfectly align with the standard name-entry structure. 

The dumper automatically detects:
* **FNamePool vs. Legacy String Tables:** Supports both modern `FNamePool` (UE4.23+) and older legacy `TNameEntryArray` structures.
* **`Blocks` Offset Detection:** Inside `FNamePool`, it auto-detects the offset of the inner `Blocks[]` pointer array by probing candidate offsets and verifying that index `0` resolves and decodes to `"None"`.
* **Case-Preserving Support:** Supports case-preserving strings (`bIsCasePreserving`).
* **`AppendString` Support:** Detects and resolves `FName::AppendString` function address for direct execution.
