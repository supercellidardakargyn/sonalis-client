plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

val releaseVersionName = providers.environmentVariable("SONALIS_VERSION_NAME").orElse("5.2.0")
val releaseVersionCode = providers.environmentVariable("SONALIS_VERSION_CODE").orElse("50200")
val signingStorePath = providers.environmentVariable("SONALIS_ANDROID_KEYSTORE_PATH")
val signingStorePassword = providers.environmentVariable("SONALIS_ANDROID_KEYSTORE_PASSWORD")
val signingKeyAlias = providers.environmentVariable("SONALIS_ANDROID_KEY_ALIAS")
val signingKeyPassword = providers.environmentVariable("SONALIS_ANDROID_KEY_PASSWORD")
val signingReady = listOf(
    signingStorePath.orNull,
    signingStorePassword.orNull,
    signingKeyAlias.orNull,
    signingKeyPassword.orNull,
).all { !it.isNullOrBlank() }

android {
    namespace = "tr.sonalis.mobile"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "tr.sonalis.mobile"
        minSdk = 29
        targetSdk = 35
        versionCode = releaseVersionCode.get().toInt()
        versionName = releaseVersionName.get()

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++20")
                arguments += listOf("-DANDROID_STL=c++_shared", "-DBUILD_TESTING=OFF")
            }
        }
        ndk { abiFilters += listOf("arm64-v8a", "x86_64") }
    }


    signingConfigs {
        if (signingReady) {
            create("sonalisRelease") {
                storeFile = file(signingStorePath.get())
                storePassword = signingStorePassword.get()
                keyAlias = signingKeyAlias.get()
                keyPassword = signingKeyPassword.get()
                enableV1Signing = false
                enableV2Signing = true
                enableV3Signing = true
                enableV4Signing = true
            }
        }
    }

    buildTypes {
        debug {
            applicationIdSuffix = ".debug"
            versionNameSuffix = "-debug"
        }
        release {
            if (signingReady) {
                signingConfig = signingConfigs.getByName("sonalisRelease")
            }
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.31.1"
        }
    }
    buildFeatures { buildConfig = true }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    packaging { jniLibs.useLegacyPackaging = false }
}

gradle.taskGraph.whenReady {
    val requestsRelease = allTasks.any { task ->
        task.name.contains("Release", ignoreCase = true)
    }
    if (requestsRelease && !signingReady) {
        throw GradleException(
            "Release imzasi eksik. SONALIS_ANDROID_KEYSTORE_PATH, " +
                "SONALIS_ANDROID_KEYSTORE_PASSWORD, SONALIS_ANDROID_KEY_ALIAS ve " +
                "SONALIS_ANDROID_KEY_PASSWORD ayarlanmalidir."
        )
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
}
