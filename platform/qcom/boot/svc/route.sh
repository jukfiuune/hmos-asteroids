# No direct scanout while the boot animation runs (HarmonyOS init service asteroids_route, once, at post-init): with
# it, render_service stops presenting the animation after its first frames (4 commits in 12 s, a black screen until
# the desktop). The composer reads this file every 2 s; booted.sh removes it once the desktop is up.
echo 0 > /dev/sheng-display-route-mode
