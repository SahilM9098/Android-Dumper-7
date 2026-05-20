import java.util.zip.ZipFile
import java.io.File
plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "com.sahilm9098.arkmodmenu"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.sahilm9098.arkmodmenu"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"

        ndk {
            abiFilters += "arm64-v8a"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
}

dependencies {
}

// ─────────────────────────────────────────────────────────────
//  TASK 1 — Full pipeline
//  assembleRelease → extract DEX → regenerate dex_payload.h
//  → assembleRelease again → copy libmenu.so to /output
// ─────────────────────────────────────────────────────────────

tasks.register("buildPayloadHeader") {
    group = "build"
    description = "Extract classes.dex from the freshly-built APK and write src/main/cpp/dex_payload.h."
    dependsOn("assembleRelease")

    val apkPath = layout.buildDirectory.file("outputs/apk/release/app-release-unsigned.apk")
    val cppDir  = file("src/main/cpp")

    doLast {
        val apkFile = apkPath.get().asFile
        if (!apkFile.exists()) throw GradleException("APK not found — run assembleRelease first.")

        val zipFile  = ZipFile(apkFile)
        val dexEntry = zipFile.getEntry("classes.dex")
            ?: throw GradleException("classes.dex not found in APK!")

        val dexBytes = zipFile.getInputStream(dexEntry).readBytes()
        zipFile.close()

        val headerFile = File(cppDir, "dex_payload.h")
        val sb = StringBuilder()
        sb.append("#pragma once\n\n")
        sb.append("const unsigned char payload_dex[] = {\n    ")
        for (i in dexBytes.indices) {
            sb.append(String.format("0x%02x", dexBytes[i]))
            if (i < dexBytes.size - 1) sb.append(", ")
            if ((i + 1) % 16 == 0) sb.append("\n    ")
        }
        sb.append("\n};\n")
        sb.append("const unsigned int payload_dex_size = ${dexBytes.size};\n")
        headerFile.writeText(sb.toString())

        println("dex_payload.h written (${dexBytes.size} bytes)")
    }
}

tasks.register<GradleBuild>("recompileLibraryWithPayload") {
    group       = "build"
    description = "Second assembleRelease pass — compiles the native library with the refreshed dex_payload.h."
    tasks       = listOf(":app:assembleRelease")
    startParameter.projectProperties = gradle.startParameter.projectProperties
    mustRunAfter("buildPayloadHeader")
}

val outputDir = rootProject.layout.projectDirectory.dir("output")

/** Full pipeline — use this whenever Java/Kotlin sources change. */
tasks.register("buildFullPipeline") {
    group       = "build"
    description = "[FULL] APK compile → DEX → dex_payload.h → native library compile → /output"
    dependsOn("buildPayloadHeader", "recompileLibraryWithPayload")

    doLast {
        val libSrc = layout.buildDirectory
            .file("intermediates/stripped_native_libs/release/stripReleaseDebugSymbols/out/lib/arm64-v8a/libmenu.so")
            .get().asFile
        val libDst = outputDir.asFile.also { it.mkdirs() }

        if (!libSrc.exists()) throw GradleException("libmenu.so not found after second assemble pass.")
        libSrc.copyTo(File(libDst, "libmenu.so"), overwrite = true)

        println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
        println("Output → ${libDst.absolutePath}")
        println("  libmenu.so  ← payload (full rebuild)")
        println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    }
}


// ─────────────────────────────────────────────────────────────
//  TASK 2 — Native-only recompile
//  Skips APK build + DEX extraction entirely.
//  Use when only C++ sources changed and dex_payload.h is
//  already up-to-date from a previous full pipeline run.
// ─────────────────────────────────────────────────────────────

tasks.register("buildNativeOnly") {
    group       = "build"
    description = "[NATIVE ONLY] Recompile native library with existing dex_payload.h → /output. No Java rebuild."
    dependsOn("assembleRelease")        // still needs a full assemble to invoke CMake/ndk-build

    doLast {
        val headerFile = file("src/main/cpp/dex_payload.h")
        if (!headerFile.exists()) throw GradleException(
            "dex_payload.h missing — run buildFullPipeline at least once first."
        )

        val libSrc = layout.buildDirectory
            .file("intermediates/stripped_native_libs/release/stripReleaseDebugSymbols/out/lib/arm64-v8a/libmenu.so")
            .get().asFile
        val libDst = outputDir.asFile.also { it.mkdirs() }

        if (!libSrc.exists()) throw GradleException("libmenu.so not found after assemble.")
        libSrc.copyTo(File(libDst, "libmenu.so"), overwrite = true)

        println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
        println("Output → ${libDst.absolutePath}")
        println("  libmenu.so  ← payload (native-only rebuild)")
        println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    }
}
