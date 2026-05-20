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
| **Code-Ref** | Exhaustive Instruction Scan | Walks the entire executable `.text` segment instruction-by-instruction. It decodes all AArch64 `ADRP+ADD`/`ADRP+LDR` pairs, tracks dynamic register materializations, and whenever a writeable-segment address target is encountered, validates it with a layout probe. This is extremely robust and **engine-agnostic** since it does not depend on literals surviving compilation. |
| **BSS Scan** | Structural Brute Scan | A fast, exhaustive walk of `libUE4.so`'s writable segments (`.data` and `.bss` PT_LOAD segments) at 8-byte strides. It runs layout validations on every address. Because validation is highly strict, non-UObject targets fail in 1-2 memory reads, making it cheap and safe. |
| **DeepScan** | Cross-Library BSS Scan | Performs a structural BSS scan across **all loaded shared libraries** on the Android device. This is crucial for modern UE5 games where engine modules are split, and `GUObjectArray` may reside in a sibling library (like `libCoreUObject.so` instead of `libUnreal.so`). |

#### 🛡️ GUObjectArray Validation (`Probe`)
Every candidate address found by the scanner is run through strict layout validations (`ObjectArray::Probe`). A candidate is only accepted if:
* The chunk layout is perfectly aligned and within bounds.
* The items at index `0` and `1` have an internal index matching their array position.
* The first object's virtual function table (`vtable`) points to an executable memory segment in loaded modules.

---

### 2. GNames (FNamePool / FNames) Search Strategies

To find `FNamePool` (which stores the strings representing Unreal Engine object and type names), the engine exposes the following strategies:

| Strategy | Search Type | Detailed Logic & Operations |
| :--- | :--- | :--- |
| **Symbol** | Dynamic Symbol Lookup | Tries resolving known symbol patterns (like `GNames`, `FNamePool`, etc.) from dynsym or disk symbol tables, running validations on each candidate. |
| **Func-Walk** | Control Flow Disassembly | 1. Resolves high-probability FName constructor/conversion symbol anchors from the dynamic/disk tables (e.g., `FName::ToString`, `FName::AppendString`, and constructors like `_ZN5FNameC1EPKc9EFindName`).<br>2. Disassembles a window of instructions (`kFuncWalkBytes`) in the target functions.<br>3. Dynamically decodes `ADRP+ADD` instructions and follows branches (`B`/`BL`) one hop deep to trace and extract candidate `FNamePool` materialization addresses inside secondary child functions (e.g., `FNamePool::Find`). |
| **String-Ref** | Literal materialization trace | Scans `.rodata` for strings embedded in FName functions (such as `"Failed to find name '%s'"`, `"Name table corrupted"`, `"Reserve %d entries"`, or `"NamePoolData"`). It traces their code materialization references in `.text` to isolate nearby `FNamePool` global variables. |
| **Code-Ref** | Exhaustive Instruction Scan | Instruction-by-instruction scan of the executable memory space to identify `ADRP+ADD` or `ADRP+LDR` instructions resolving to writeable data segments, running name validation checks on each target. |
| **BSS Scan** | Structural Brute Scan | Walks `.data` and `.bss` segments at 8-byte strides and runs strict verification checks. |

#### 🛡️ GNames Validation (`Probe`)
`FNamePool` candidates are validated dynamically by checking for a valid `"None"` entry at structural `Blocks[0]`, ensuring that character boundaries and length headers perfectly align with the standard name-entry structure.
