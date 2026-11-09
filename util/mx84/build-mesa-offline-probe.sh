#!/bin/sh
set -eu

source_dir=$(CDPATH= cd -- "${1:?usage: build-mesa-offline-probe.sh SDK_SOURCE BUILD_DIR}" && pwd)
build_dir=${2:?usage: build-mesa-offline-probe.sh SDK_SOURCE BUILD_DIR}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
pin=d3cd8a99bddbcd6505cb97c116cb5ff0f8c0f7b5
[ "$(git -C "$source_dir" rev-parse HEAD)" = "$pin" ] || {
    echo "MESA checkout does not match tested pin $pin" >&2; exit 1;
}
git -C "$source_dir" diff --quiet HEAD -- || {
    echo "MESA checkout has local changes; use the clean pinned source" >&2; exit 1;
}
cmake -S "$source_dir" -B "$build_dir" -G Ninja \
    -DMESA_WRAP=OFF -DBUILD_MESA_DEMO=OFF -DBUILD_SHARED_LIBS=OFF -Dvsc7425_static=ON
cmake --build "$build_dir" --target vsc7425_static -j2
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unknown-pragmas \
    -D_DEFAULT_SOURCE -DVTSS_OPSYS_LINUX -DVTSS_CHIP_SPARX_III_18 \
    -DVTSS_OPT_SYMREG=1 -DLMU_BOOL_IS_UINT8 \
    -I"$source_dir/include" -I"$source_dir/utils/include" -I"$source_dir/me/include" \
    -I"$source_dir/base/luton26" \
    -c "$here/mesa-offline-probe.c" -o "$build_dir/mx84-offline-probe.o"
cc -std=c11 -Wall -Wextra -Werror -Wno-unknown-pragmas -pthread \
    -D_DEFAULT_SOURCE -DVTSS_OPSYS_LINUX -DVTSS_CHIP_SPARX_III_18 \
    -DVTSS_OPT_SYMREG=1 -DLMU_BOOL_IS_UINT8 \
    -I"$source_dir/include" -I"$source_dir/utils/include" \
    -c "$here/mesa-transport.c" -o "$build_dir/mx84-mesa-transport.o"
cc -std=c11 -Wall -Wextra -Werror -pthread \
    -c "$here/mdio-window.c" -o "$build_dir/mx84-mdio-window.o"
cc -std=c11 -Wall -Wextra -Werror -Wno-unknown-pragmas -pthread \
    -D_DEFAULT_SOURCE -DVTSS_OPSYS_LINUX -DVTSS_CHIP_SPARX_III_18 \
    -DVTSS_OPT_SYMREG=1 -DLMU_BOOL_IS_UINT8 \
    -I"$source_dir/include" -I"$source_dir/utils/include" \
    -I"$source_dir/me/include" -I"$source_dir/mepa/include" \
    -c "$here/mesa-phy-identity.c" -o "$build_dir/mx84-mesa-phy-identity.o"
cc -std=c11 -Wall -Wextra -Werror -Wno-unknown-pragmas -D_DEFAULT_SOURCE \
    -I"$source_dir/me/include" -I"$source_dir/mepa/include" \
    -c "$here/phy-identity.c" -o "$build_dir/mx84-phy-identity.o"
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unknown-pragmas -pthread \
    -D_DEFAULT_SOURCE -DVTSS_OPSYS_LINUX -DVTSS_CHIP_SPARX_III_18 \
    -DVTSS_OPT_SYMREG=1 -DLMU_BOOL_IS_UINT8 \
    -I"$source_dir/include" -I"$source_dir/utils/include" -I"$source_dir/me/include" \
    -I"$source_dir/base/luton26" \
    -c "$here/test-phy-stack.c" -o "$build_dir/mx84-test-phy-stack.o"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    -c "$here/linux-mii.c" -o "$build_dir/mx84-linux-mii.o"
c++ "$build_dir/mx84-offline-probe.o" "$build_dir/mx84-mesa-transport.o" \
    "$build_dir/mx84-test-phy-stack.o" "$build_dir/mx84-linux-mii.o" \
    "$build_dir/mx84-mesa-phy-identity.o" "$build_dir/mx84-phy-identity.o" \
    "$build_dir/mx84-mdio-window.o" "$build_dir/mesa/libvsc7425.a" \
    -lpthread -ldl -o "$build_dir/mx84-offline-probe"
"$build_dir/mx84-offline-probe"
cc -std=c11 -Wall -Wextra -Werror -pthread \
    "$here/mdio-window.c" "$here/test-mdio-window.c" -o "$build_dir/mx84-mdio-window-test"
"$build_dir/mx84-mdio-window-test"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pthread \
    "$here/mdio-window.c" "$here/linux-mii.c" "$here/test-linux-mii.c" \
    -o "$build_dir/mx84-linux-mii-test"
"$build_dir/mx84-linux-mii-test"
cmake --build "$build_dir" --target mepa_caracal_target lm_utils -j2
cc -std=c11 -Wall -Wextra -Werror -Wno-unknown-pragmas -D_DEFAULT_SOURCE \
    -I"$source_dir/me/include" -I"$source_dir/mepa/include" \
    "$here/mepa-offline-probe.c" "$here/phy-identity.c" "$build_dir/mepa/libmepa_caracal.a" \
    "$build_dir"/utils/CMakeFiles/lm_utils.dir/src/*.o \
    -lpthread -o "$build_dir/mx84-offline-phy-probe"
"$build_dir/mx84-offline-phy-probe"
