#!/bin/sh
# Builds the iOS app with your copy of the game (on a Mac):
#   recomp/ios/build.sh "<folder with daybreak.exe and d3dx9_25.dll>"
# Translates the two modules into recomp/gen (kept out of git: it is made from the game),
# then generates the Xcode project. Open DaybreakDX.xcodeproj, set your signing team
# and run it on your device. Needs Python 3 with Capstone (pip3 install capstone) and
# XcodeGen (brew install xcodegen).
set -e
here=$(cd "$(dirname "$0")" && pwd)
game=${1:?usage: build.sh <folder with daybreak.exe>}
cd "$here/.."
rm -rf gen
python3 tools/lift.py "$game/daybreak.exe" gen --prefix F
python3 tools/lift.py "$game/d3dx9_25.dll" gen --prefix X --base 0x00A00000
cd ios
xcodegen generate
echo "Now open $here/DaybreakDX.xcodeproj in Xcode."
