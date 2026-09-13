#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-run}"
APP_NAME="AstroLibrary"
BUNDLE_ID="com.astrolibrary.local"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
BUILD_DIR="$ROOT_DIR/build/swift"
DIST_DIR="$ROOT_DIR/dist"
APP_BUNDLE="$DIST_DIR/$APP_NAME.app"
APP_CONTENTS="$APP_BUNDLE/Contents"
APP_RESOURCES="$APP_CONTENTS/Resources"
APP_PAYLOAD="$APP_RESOURCES/AstroLibrary"
APP_BINARY="$APP_CONTENTS/MacOS/$APP_NAME"
DMG="$DIST_DIR/$APP_NAME.dmg"
LOG_FILE="$HOME/Library/Logs/AstroLibrary/server.log"
SETTINGS_FILE="$HOME/Library/Application Support/AstroLibrary/settings.json"

stop_app() {
  pkill -x "$APP_NAME" >/dev/null 2>&1 || true
  pkill -f "$APP_CONTENTS/MacOS/astrolibrary-backend serve" >/dev/null 2>&1 || true
  # Stop the previous Python helper when upgrading an already running 0.7 app.
  pkill -f "$APP_PAYLOAD/astrolibrary_server.py" >/dev/null 2>&1 || true
}

build_app() {
  "$ROOT_DIR/script/build_native.sh"
  swift build --scratch-path "$BUILD_DIR" --product "$APP_NAME"
  local build_binary
  build_binary="$(swift build --scratch-path "$BUILD_DIR" --show-bin-path)/$APP_NAME"

  rm -rf "$APP_BUNDLE"
  mkdir -p "$APP_CONTENTS/MacOS" "$APP_RESOURCES" "$APP_PAYLOAD/web"
  install -m 755 "$build_binary" "$APP_BINARY"
  if cmp -s "$APP_BINARY" "$ROOT_DIR/build/native/astrolibrary"; then
    printf "App binary must be the Swift UI executable, not the backend.\n" >&2
    exit 1
  fi
  cp "$ROOT_DIR/packaging/Info.plist" "$APP_CONTENTS/Info.plist"
  install -m 755 "$ROOT_DIR/build/native/astrolibrary" "$APP_CONTENTS/MacOS/astrolibrary-backend"
  if cmp -s "$APP_BINARY" "$APP_CONTENTS/MacOS/astrolibrary-backend"; then
    printf "App and backend executables must be distinct.\n" >&2
    exit 1
  fi
  "$APP_CONTENTS/MacOS/astrolibrary-backend" --self-test
  cp "$ROOT_DIR/native/THIRD_PARTY.md" "$APP_PAYLOAD/THIRD_PARTY.md"
  mkdir -p "$APP_PAYLOAD/licenses"
  cp "$ROOT_DIR/native/vendor/"*.LICENSE* "$APP_PAYLOAD/licenses/"
  cp -R "$ROOT_DIR/web/." "$APP_PAYLOAD/web/"

  xcrun actool "$ROOT_DIR/macapp/Resources/Assets.xcassets" \
    --compile "$APP_RESOURCES" \
    --platform macosx \
    --minimum-deployment-target 14.0 \
    --app-icon AppIcon \
    --output-partial-info-plist "$DIST_DIR/AppIcon-Info.plist" >/dev/null
  codesign --force --sign - "$APP_CONTENTS/MacOS/astrolibrary-backend" >/dev/null
  codesign --force --deep --sign - "$APP_BUNDLE" >/dev/null
}

open_app() {
  /usr/bin/open -n "$APP_BUNDLE"
}

create_dmg() {
  local stage="$DIST_DIR/$APP_NAME-dmg"
  rm -rf "$stage" "$DMG"
  mkdir -p "$stage"
  cp -R "$APP_BUNDLE" "$stage/"
  ln -s /Applications "$stage/Applications"
  hdiutil create -volname "$APP_NAME" -srcfolder "$stage" -ov -format UDZO "$DMG" >/dev/null
  rm -rf "$stage"
  printf 'DMG 已生成：%s\n' "$DMG"
}

stop_app
build_app

case "$MODE" in
  run)
    open_app
    ;;
  --debug|debug)
    exec lldb -- "$APP_BINARY"
    ;;
  --logs|logs)
    open_app
    mkdir -p "$(dirname "$LOG_FILE")"
    touch "$LOG_FILE"
    exec tail -F "$LOG_FILE"
    ;;
  --telemetry|telemetry)
    open_app
    exec /usr/bin/log stream --info --style compact --predicate "process == \"$APP_NAME\" OR subsystem == \"$BUNDLE_ID\""
    ;;
  --verify|verify)
    open_app
    port="$(/usr/bin/plutil -extract port raw -o - "$SETTINGS_FILE" 2>/dev/null || true)"
    port="${port:-8765}"
    for _ in {1..80}; do
      if pgrep -x "$APP_NAME" >/dev/null && /usr/bin/curl -fsS --max-time 1 "http://127.0.0.1:$port/api/health" >/dev/null 2>&1; then
        printf '%s App 与本地服务验证通过：http://127.0.0.1:%s\n' "$APP_NAME" "$port"
        exit 0
      fi
      sleep 0.25
    done
    printf '%s 启动验证失败；请查看 %s。\n' "$APP_NAME" "$LOG_FILE" >&2
    exit 1
    ;;
  --package|package)
    create_dmg
    ;;
  *)
    printf 'usage: %s [run|--debug|--logs|--telemetry|--verify|--package]\n' "$0" >&2
    exit 2
    ;;
esac
