#!/bin/bash
# Gamesdk build driver script
# Usage:
# ./build.sh
#   Builds the gamesdk with Swappy, Tuning Fork and Oboe (no samples)
# ./build.sh samples
#   Builds the gamesdk with Swappy and the Swappy samples
# ./build.sh full
#   Builds the gamesdk with Swappy, Tuning Fork, Oboe and all samples
# ./build.sh tests
#   Runs connected instrumentation tests on attached device
# ./build.sh testApks
#   Assembles instrumentation and debug test APKs and exports to dist directory
# ./build.sh hostUnitTests
#   Runs the host C++ unit tests (swappy_host_test) with prebuilt JDK environment

# ./build.sh coverageBundle: write the .gcno bundle from testApks to DIST_DIR.
# ./build.sh coverageTests: run the tools/coverage unit and corpus tests.
# ./build.sh hostCoverageReport (or ./build.sh hostUnitTests --coverage):
#   write the host coverage report to DIST_DIR.

set -e # Exit on error

# Set up the environment
PREBUILTS_DIR="$(pwd)/../prebuilts"
IS_AOSP_CHECKOUT=false
if [ ! -d "$PREBUILTS_DIR" ] && [ -d "$(pwd)/../../../prebuilts" ]; then
    PREBUILTS_DIR="$(pwd)/../../../prebuilts"
    IS_AOSP_CHECKOUT=true
fi
export PREBUILTS_DIR
export IS_AOSP_CHECKOUT

export ANDROID_HOME=$PREBUILTS_DIR/sdk
unset  ANDROID_SDK_ROOT
unset  ANDROID_NDK_HOME
export BUILDBOT_SCRIPT=true
export BUILDBOT_CMAKE=$PREBUILTS_DIR/cmake/linux-x86
# Prepend prebuilts to PATH so they override system host binaries
export PATH="$PREBUILTS_DIR/ninja/linux-x86/:$PREBUILTS_DIR/build-tools/linux-x86/bin/:$PREBUILTS_DIR/cmake/linux-x86/bin/:$PATH"

# Dynamically find the most recent AOSP internal host clang prebuilt
CLANG_BASE_DIR="$PREBUILTS_DIR/clang/host/linux-x86"
if [ -d "$CLANG_BASE_DIR" ]; then
    # Grab the latest clang-r* directory by sorting version numbers
    LATEST_CLANG_DIR=$(ls -1d "$CLANG_BASE_DIR"/clang-r* 2>/dev/null | sort -V | tail -n 1)
    if [ -n "$LATEST_CLANG_DIR" ]; then
        CLANG_PREBUILTS_DIR="$LATEST_CLANG_DIR/bin"
        if [ -d "$CLANG_PREBUILTS_DIR" ]; then
            export CC="$CLANG_PREBUILTS_DIR/clang"
            export CXX="$CLANG_PREBUILTS_DIR/clang++"
        fi
    fi
fi

echo "=== 🕵️ Advanced Compiler Diagnostics ==="
echo "Working Directory: $(pwd)"
echo "Target Clang Dir: $CLANG_PREBUILTS_DIR"
echo "System OS & Kernel: $(uname -a)"

# 1. Inspect what prebuilt compilers are actually checked out
echo -e "\n--- 📂 Available Clang Prebuilts ---"
if [ -d "$PREBUILTS_DIR/clang/host/linux-x86/" ]; then
    ls -F "$PREBUILTS_DIR/clang/host/linux-x86/"
else
    echo "Parent directory $PREBUILTS_DIR/clang/host/linux-x86/ does not exist!"
    # Let's see what exists in prebuilts at all
    ls -F "$PREBUILTS_DIR/"
fi

# 2. Check compiler binary executability and Shared Libs
echo -e "\n--- ⚙️ Compiler Execution & Dependencies ---"
if [ -f "$CXX" ]; then
    echo "Binary found: $CXX"
    echo "File info:"
    file "$CXX" || echo "file command failed"

    echo -e "\nTesting execution ($CXX --version):"
    $CXX --version || echo "Failed to execute $CXX"

    # Check if GLIBC / dynamic library linkage is satisfied
    echo -e "\nChecking Dynamic Linker dependencies (ldd):"
    ldd "$CXX" || echo "ldd command failed"
else
    echo "Binary DOES NOT exist at: $CXX"
fi

# 3. Check for default system compilers
echo -e "\n--- 🖥️ System Fallback Compilers ---"
echo "Default CC: $CC"
echo "Default CXX: $CXX"
if command -v clang++ >/dev/null 2>&1; then
    echo "Found system clang++: $(command -v clang++)"
    clang++ --version
else
    echo "No system clang++ found."
fi

if command -v g++ >/dev/null 2>&1; then
    echo "Found system g++: $(command -v g++)"
    g++ --version
else
    echo "No system g++ found."
fi

echo "========================================"
if [ "$(uname)" == "Darwin" ]; then
    : # Do nothing but skip the next condition so we don't get a bash warning on macos
elif [ "$(expr substr $(uname -s) 1 5)" == "Linux" ]; then
    # Do only for GNU/Linux platform
    if [ -d "$PREBUILTS_DIR/jdk/jdk17/linux-x86" ]; then
        export JAVA_HOME=$PREBUILTS_DIR/jdk/jdk17/linux-x86
    elif [ -d "$PREBUILTS_DIR/jdk/jdk21/linux-x86" ]; then
        export JAVA_HOME=$PREBUILTS_DIR/jdk/jdk21/linux-x86
    fi
    if [ -n "$JAVA_HOME" ]; then
        export PATH="$JAVA_HOME/bin:$PATH"
    fi
fi

if [ "$IS_AOSP_CHECKOUT" = "true" ]; then
    if [ -f "$PREBUILTS_DIR/ndk/current/source.properties" ]; then
        AGDK_NDK_VERSION=$(grep -E "^Pkg\.Revision" "$PREBUILTS_DIR/ndk/current/source.properties" | cut -d'=' -f2 | xargs)
        export ANDROID_NDK_HOME="$PREBUILTS_DIR/ndk/current"
        export ANDROID_NDK="$ANDROID_NDK_HOME"
    fi
fi

if [[ -z "$AGDK_NDK_VERSION" ]]; then
    AGDK_NDK_VERSION=$(grep -E -o "[0-9]+\.[0-9]+\.[0-9]+" ndk_version.gradle | head -n 1)
fi

# Only invoke sdkmanager for the standalone checkout (not when in AOSP).
if [ "$IS_AOSP_CHECKOUT" = "false" ]; then
    sdkmanager_path="$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager"

    if [ ! -f "$sdkmanager_path" ]; then
        pushd $ANDROID_HOME
        mkdir -p cmdline-tools/latest && \
            curl -o cmdline-tools/latest/sdk-tools.zip https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip && \
            unzip cmdline-tools/latest/sdk-tools.zip -d cmdline-tools/latest && \
            mv cmdline-tools/latest/cmdline-tools/* cmdline-tools/latest/ && \
            rm -rf cmdline-tools/latest/cmdline-tools && \
            rm cmdline-tools/latest/sdk-tools.zip
        popd
    fi
    echo yes | $sdkmanager_path "platform-tools"
    echo yes | $sdkmanager_path "platforms;android-35"
    echo yes | $sdkmanager_path "platforms;android-31"
    echo yes | $sdkmanager_path "build-tools;35.0.0"
    echo yes | $sdkmanager_path "ndk;$AGDK_NDK_VERSION"
else
    echo "Skipping sdkmanager invocation (AOSP prebuilts detected at $PREBUILTS_DIR)."
fi

# Prepend the NDK's hermetic Python 3.11 and LLVM tools to PATH so CI containers
# with older system Python (e.g. Ubuntu 14.04 Python 3.4) run coverage scripts cleanly.
for ndk_llvm_dir in \
    "$ANDROID_HOME/ndk/$AGDK_NDK_VERSION/toolchains/llvm/prebuilt/linux-x86_64" \
    "$PREBUILTS_DIR/ndk/r27/toolchains/llvm/prebuilt/linux-x86_64" \
    "$PREBUILTS_DIR/ndk/current/toolchains/llvm/prebuilt/linux-x86_64"; do
    if [ -d "$ndk_llvm_dir/python3/bin" ]; then
        export PATH="$ndk_llvm_dir/python3/bin:$ndk_llvm_dir/bin:$PATH"
        break
    fi
done

# Use the distribution path given to the script by the build bot in DIST_DIR. Otherwise,
# build in the default location.
if [[ -z $DIST_DIR ]]
then
    dist_dir=$(pwd)/../dist
else
    dist_dir=$DIST_DIR
    # Install the Maven Central GCS mirror init script so standalone sample and
    # testapp Gradle builds do not hit repo.maven.apache.org IP rate limits.
    gradle_init_dir="${GRADLE_USER_HOME:-$HOME/.gradle}/init.d"
    mkdir -p "$gradle_init_dir"
    cp "$(pwd)/gradle/maven-central-mirror.init.gradle" \
        "$gradle_init_dir/gamesdk-maven-central-mirror.init.gradle"
fi

package_coverage_bundle() {
    # Write .gcno notes and the manifest for CI post-processing.
    python3 tools/coverage bundle \
        --out "$dist_dir/agdk_coverage_bundle.tar.gz" --diff-base=HEAD~1
    # An instrumentation regression yields an empty bundle, not an error.
    python3 tools/coverage/check_coverage_artifacts.py \
        bundle "$dist_dir/agdk_coverage_bundle.tar.gz" \
        --module games-frame-pacing --module game-activity \
        --module game-text-input --module games-controller
    # Temporary alias for the deployed presubmit analyzer until it switches to
    # agdk_coverage_bundle.tar.gz (b/573084056).
    cp "$dist_dir/agdk_coverage_bundle.tar.gz" "$dist_dir/swappy_coverage_bundle.tar.gz"
}

run_coverage_tests() {
    # Unit tests plus the llvm-cov corpus regression test. The corpus test must
    # run (not skip) on CI, where the NDK llvm-cov is on PATH.
    AGDK_COVERAGE_REQUIRE_LLVM_COV=1 python3 -B -m unittest discover \
        -s tools/coverage/tests -t tools/coverage
}

generate_host_coverage_report() {
    if [[ ! -d ../.temp/host/.cmake || ! -f ../out_game_activity/reports/jacoco/jacocoTestReport/jacocoTestReport.xml ]]; then
        echo "error: host coverage inputs not found; run './build.sh hostUnitTests' first" >&2
        exit 1
    fi
    python3 tools/coverage report \
        --out "$dist_dir/host_coverage" \
        --native-build-dir ../.temp/host/.cmake \
        --diff-base=HEAD~1 \
        --jacoco-xml="../out_game_activity/reports/jacoco/jacocoTestReport/jacocoTestReport.xml"
    # A path-mapping regression yields an empty report, not an error.
    python3 tools/coverage/check_coverage_artifacts.py \
        summary "$dist_dir/host_coverage/coverage_summary.json" \
        --language cpp --language java
}

## Build the Game SDK distribution zip and the zips for Maven AARs
if [[ $1 == "full" ]]
then
    package_name=fullsdk
    ./gradlew packageZip -Plibraries=swappy,tuningfork,game_activity,game_text_input,paddleboat,memory_advice -PincludeSampleSources -PincludeSampleArtifacts -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew packageMavenZip -Plibraries=swappy          -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew packageMavenZip -Plibraries=tuningfork      -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew packageMavenZip -Plibraries=game_activity   -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew packageMavenZip -Plibraries=game_text_input -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew packageMavenZip -Plibraries=paddleboat      -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew packageMavenZip -Plibraries=memory_advice   -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew jetpadJson -Plibraries=swappy,tuningfork,game_activity,game_text_input,paddleboat,memory_advice -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew spdxSbom generateAttestationManifest -PpackageName=$package_name -PdistPath="$dist_dir" -Plibraries=swappy,tuningfork,game_activity,game_text_input,paddleboat,memory_advice
elif [[ $1 == "samples" ]]
then
    package_name=gamesdk
    ./gradlew packageZip      -Plibraries=swappy -PincludeSampleSources -PincludeSampleArtifacts -PdistPath="$dist_dir"
    ./gradlew packageMavenZip -Plibraries=swappy -PdistPath="$dist_dir"
elif [[ $1 == "maven-only" ]]
then
    # Only the Maven artifacts for Jetpack
    package_name=gamesdk-maven
    ./gradlew packageMavenZip -Plibraries=swappy          -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew packageMavenZip -Plibraries=tuningfork      -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew packageMavenZip -Plibraries=game_activity   -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew packageMavenZip -Plibraries=game_text_input -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew packageMavenZip -Plibraries=paddleboat      -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew packageMavenZip -Plibraries=memory_advice   -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew jetpadJson -Plibraries=swappy,tuningfork,game_activity,game_text_input,paddleboat,memory_advice -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew spdxSbom generateAttestationManifest -PpackageName=$package_name -PdistPath="$dist_dir" -Plibraries=swappy,tuningfork,game_activity,game_text_input,paddleboat,memory_advice
elif [[ $1 == "tests" ]]
then
    export GRADLE_OPTS="-Dorg.gradle.jvmargs=\"-Xmx4096m -XX:MaxMetaspaceSize=1024m -XX:-UseContainerSupport\" $GRADLE_OPTS"
    package_name=gamesdk-tests
    ./gradlew :game-controller:connectedAndroidTest -Plibraries=paddleboat -PincludeSampleSources -PincludeSampleArtifacts -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew :game-activity:connectedAndroidTest -Plibraries=game_activity -PincludeSampleSources -PincludeSampleArtifacts -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew :game-text-input:connectedAndroidTest -Plibraries=game_text_input,game_activity -PincludeSampleSources -PincludeSampleArtifacts -PdistPath="$dist_dir" -PpackageName=$package_name
    exit
elif [[ $1 == "testApks" ]]
then
    export GRADLE_OPTS="-Dorg.gradle.jvmargs=\"-Xmx4096m -XX:MaxMetaspaceSize=1024m -XX:-UseContainerSupport\" $GRADLE_OPTS"
    package_name=gamesdk-tests
    mkdir -p "$dist_dir/$package_name/apks/test"

    ./gradlew :games-controller:assembleAndroidTest :games-controller:assembleDebug \
              :game-activity:assembleAndroidTest :game-activity:assembleDebug \
              :game-text-input:assembleAndroidTest :game-text-input:assembleDebug \
              -Plibraries=paddleboat,game_activity,game_text_input \
              -PincludeSampleSources -PincludeSampleArtifacts \
              -PenableCoverage=true \
              -PdistPath="$dist_dir" -PpackageName=$package_name

    pushd test/swappy/testapp
    ../../../gradlew :app:assembleDebug :app:assembleAndroidTest -PenableCoverage=true
    popd

    find ../out_* -path "*/outputs/apk/*/*.apk" -exec cp -v {} "$dist_dir/$package_name/apks/test/" \;
    cp -v test/swappy/testapp/app/build/outputs/apk/debug/app-debug.apk \
        "$dist_dir/$package_name/apks/test/swappy-debug.apk"
    cp -v test/swappy/testapp/app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk \
        "$dist_dir/$package_name/apks/test/swappy-debug-androidTest.apk"
    cp -v test/tradefed/*.config "$dist_dir/$package_name/apks/test/"
    (cd "$dist_dir/$package_name/apks/test" && zip -j "$dist_dir/androidTest.zip" *.apk *.config)
    cp -v "$dist_dir/androidTest.zip" "$dist_dir/$package_name/androidTest.zip"

    # Run coverageBundle in testApks while CI runs only testApks in this
    # container and ATP pairs .gcda files with these .gcno notes.
    package_coverage_bundle
    exit
elif [[ $1 == "coverageBundle" ]]
then
    package_coverage_bundle
    exit
elif [[ $1 == "hostUnitTests" ]]
then
    find ../.temp/host/.cmake -name "*.gcda" -delete 2>/dev/null || true
    ./gradlew hostUnitTests \
        :game-activity:testDebugUnitTest :game-activity:jacocoTestReport \
        :buildSrc:test \
        -Pndk=$AGDK_NDK_VERSION -PenableCoverage=true --stacktrace
    run_coverage_tests
    if [[ $2 == "--coverage" ]]
    then
        generate_host_coverage_report
    fi
    exit
elif [[ $1 == "coverageTests" ]]
then
    run_coverage_tests
    exit
elif [[ $1 == "hostCoverageReport" ]]
then
    generate_host_coverage_report
    exit
else
    # The default is to build the express zip
    package_name=gamesdk-express
    ./gradlew packageZip -Plibraries=swappy,tuningfork,game_activity,game_text_input,paddleboat,memory_advice -PincludeSampleSources -PincludeSampleArtifacts -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew packageMavenZip -Plibraries=swappy          -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew packageMavenZip -Plibraries=tuningfork      -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew packageMavenZip -Plibraries=game_activity   -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew packageMavenZip -Plibraries=game_text_input -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew packageMavenZip -Plibraries=paddleboat      -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew packageMavenZip -Plibraries=memory_advice   -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew jetpadJson -Plibraries=swappy,tuningfork,game_activity,game_text_input,paddleboat,memory_advice -PdistPath="$dist_dir" -PpackageName=$package_name
    ./gradlew spdxSbom generateAttestationManifest -PpackageName=$package_name -PdistPath="$dist_dir" -Plibraries=swappy,tuningfork,game_activity,game_text_input,paddleboat,memory_advice
fi

if [[ $1 != "maven-only" ]]
then
    export GRADLE_OPTS="-Dorg.gradle.jvmargs=\"-XX:-UseContainerSupport\" $GRADLE_OPTS"

    mkdir -p "$dist_dir/$package_name/apks/samples"
    mkdir -p "$dist_dir/$package_name/apks/test"
    mkdir -p "$dist_dir/$package_name/apks/tools"

    # Add the tuningfork samples and apks into the Game SDK distribution zip
    pushd ./samples/tuningfork/insightsdemo/
    ./gradlew ":app:assembleDebug"
    popd
    pushd ./samples/tuningfork/experimentsdemo/
    ./gradlew ":app:assembleDebug"
    popd

    # Add tuningfork monitor app
    pushd ./games-performance-tuner/tools/TuningForkMonitor/
    ./gradlew ":app:assembleDebug"
    popd

    # Add the swappy samples
    pushd samples/bouncyball
    ./gradlew ":app:assembleDebug"
    popd
    pushd third_party/cube
    ./gradlew ":app:assembleDebug"
    popd
    pushd samples/SwappyTest
    ./gradlew ":app:assembleDebug"
    popd

    # Add the memory_advice samples
    pushd samples/memory_advice/hogger/
    ./gradlew ":app:assembleDebug"
    popd

    # Add the game controller samples
    pushd samples/game_controller/
    mkdir -p ./third-party
    pushd third-party
    if [ ! -d "imgui" ] ; then
        git clone https://github.com/ocornut/imgui -b v1.89
    fi
    popd
    popd

    pushd samples/game_controller/gameactivity
    ./gradlew ":app:assembleDebug"
    popd
    pushd samples/game_controller/nativeactivity
    ./gradlew ":app:assembleDebug"
    popd

    pushd samples/agdktunnel/third-party/glm
    if [ ! -d "glm" ] ; then
        git clone https://github.com/g-truc/glm.git
    fi
    popd
    pushd samples/agdktunnel/
    ./gradlew ":app:assembleDebug"
    popd

    # Add the game text input samples
    pushd samples/game_text_input/game_text_input_testbed
    ./gradlew ":app:assembleDebug"
    popd

    cp samples/tuningfork/insightsdemo/app/build/outputs/apk/debug/app-debug.apk \
      "$dist_dir/$package_name/apks/samples/insightsdemo.apk"
    cp samples/tuningfork/experimentsdemo/app/build/outputs/apk/debug/app-debug.apk \
      "$dist_dir/$package_name/apks/samples/experimentsdemo.apk"

    cp games-performance-tuner/tools/TuningForkMonitor/app/build/outputs/apk/debug/app-debug.apk \
      "$dist_dir/$package_name/apks/tools/tuningforkmonitor.apk"

    cp samples/game_controller/nativeactivity/app/build/outputs/apk/debug/app-debug.apk \
      "$dist_dir/$package_name/apks/samples/game_controller_nativeactivity.apk"
    cp samples/game_controller/gameactivity/app/build/outputs/apk/debug/app-debug.apk \
      "$dist_dir/$package_name/apks/samples/game_controller_gameactivity.apk"

    cp samples/bouncyball/app/build/outputs/apk/debug/app-debug.apk \
      "$dist_dir/$package_name/apks/samples/bouncyball.apk"
    cp third_party/cube/app/build/outputs/apk/debug/app-debug.apk \
      "$dist_dir/$package_name/apks/samples/cube.apk"

    cp samples/memory_advice/hogger/app/build/outputs/apk/debug/app-debug.apk \
      "$dist_dir/$package_name/apks/samples/hogger.apk"

    cp samples/agdktunnel/app/build/outputs/apk/debug/app-debug.apk \
      "$dist_dir/$package_name/apks/samples/agdktunnel.apk"

    cp samples/game_text_input/game_text_input_testbed/app/build/outputs/apk/debug/app-debug.apk \
      "$dist_dir/$package_name/apks/samples/game_text_input.apk"

    pushd $dist_dir/$package_name
    if [[ -z "$(ls -1 agdk-libraries-*.zip 2>/dev/null | grep agdk)" ]] ; then
      echo 'Could not find the zip "agdk-libraries-*.zip".'
      exit
    fi
    zip -ur agdk-libraries-*.zip "apks/samples/insightsdemo.apk"
    zip -ur agdk-libraries-*.zip "apks/samples/experimentsdemo.apk"

    zip -ur agdk-libraries-*.zip "apks/tools/tuningforkmonitor.apk"

    zip -ur agdk-libraries-*.zip "apks/samples/game_controller_nativeactivity.apk"
    zip -ur agdk-libraries-*.zip "apks/samples/game_controller_gameactivity.apk"
    zip -ur agdk-libraries-*.zip "apks/samples/bouncyball.apk"
    zip -ur agdk-libraries-*.zip "apks/samples/cube.apk"
    zip -ur agdk-libraries-*.zip "apks/samples/hogger.apk"
    zip -ur agdk-libraries-*.zip "apks/samples/agdktunnel.apk"
    zip -ur agdk-libraries-*.zip "apks/samples/game_text_input.apk"
    popd
fi

# Calculate hash of the zip file
pushd "$dist_dir/$package_name"
for ZIPNAME in agdk-libraries-*
do
    if [[ -e $ZIPNAME ]]
    then
        sha256sum $ZIPNAME > $ZIPNAME.sha256
    fi
    break
done
popd

pushd "$dist_dir/$package_name"
# Remove intermediate files that would be very costly to store
rm -rf libs prefab
# Remove other files that we don't care about and are polluting the output
rm -rf external third_party src include samples aar
popd
