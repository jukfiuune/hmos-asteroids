# Sourced by the laptop-side shell scripts: the build machine from port.conf (port.conf.example says what goes in it).
#   pc "<cmd>"                 runs a command on it, in BUILD_ROOT
#   pc_get <dest dir> <path>...  copies files from it (paths relative to BUILD_ROOT, or absolute)
_conf_repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
_host=${BUILD_HOST-}; _root=${BUILD_ROOT-}
[ -f "$_conf_repo/port.conf" ] && . "$_conf_repo/port.conf"
BUILD_HOST=${_host:-${BUILD_HOST:-local}}; BUILD_ROOT=${_root:-${BUILD_ROOT:-harmony-build}}
pc() {
    if [ "$BUILD_HOST" = local ]; then (cd ~ && cd "$BUILD_ROOT" && bash -c "$1")
    else ssh "$BUILD_HOST" "cd $BUILD_ROOT && $1"; fi
}
pc_get() {
    local dest=$1 p; shift
    for p in "$@"; do
        if [ "$BUILD_HOST" = local ]; then (cd ~ && cd "$BUILD_ROOT" && cp "$p" "$dest/")
        else case $p in /*) ;; *) p=$BUILD_ROOT/$p;; esac; scp -q "$BUILD_HOST:$p" "$dest/"; fi
    done
}
