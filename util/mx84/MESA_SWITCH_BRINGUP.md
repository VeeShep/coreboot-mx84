# Open VSC7425 initializer: offline checkpoint

2026-11-08. This is preparation, not a replacement deployed on Fry.
Management, the vendor switch daemon, and the working VLAN profile remain intact.

## Public SDK build

Microchip's MIT-licensed MESA tree supports VSC7425 / SparX-III-18 / Luton26:

- Source: <https://github.com/microchip-ung/mesa>
- Pin: `d3cd8a99bddbcd6505cb97c116cb5ff0f8c0f7b5` (2026-09-23).
- License: <https://github.com/microchip-ung/mesa/blob/d3cd8a99bddbcd6505cb97c116cb5ff0f8c0f7b5/LICENSE>
- Public APIs: `include/vtss_init_api.h`, `vtss_port_api.h`, `vtss_misc_api.h`.
- Silicon code: `base/luton26/`; reference board code: `meba/src/caracal/meba.c`.

On an Ubuntu 24.04 build container, installed CMake/Ninja (compiler and
make already present). Cloned the public SDK into
`/tmp/mx84-mesa.7aIJrc/src`, with build output in the sibling `build` directory.
No Meraki binaries are needed for this build.

From the MX84 repository, with a clean SDK checkout at that exact pin:

```sh
sh util/mx84/build-mesa-offline-probe.sh /path/to/mesa /path/to/build
```

This disables the generated MESA wrapper, reference demo, and shared libraries;
builds only `vsc7425_static`; then links/runs our software-only probe. The probe
creates/reads/destroys an SDK software instance, validates its 18-port capacity
and the 17 unique MX84 silicon-port numbers, and asserts no hardware register
callbacks are initially installed. It then temporarily injects fake callbacks
through private SDK state to check indirect-access errors, restoring the
configuration before destruction. It has no ioctl/socket backend and never calls
`vtss_init_conf_set` or `vtss_port_map_set`. Assertions cannot be disabled.

Observed result:

```text
PASS: SDK indirect read/write errors at every stage and stuck-BUSY callback escape; fake I/O only
PASS: VSC7425 software lifecycle, capacity 18, 17 unique MX84 chip ports; no hardware backend
```

Build logs: `configure.log`, `build.log`, `probe-build.log` in that container
directory. Library `build/mesa/libvsc7425.a` SHA-256:
`d7e4b39e5aca6fbaa5ca42ee7a0ec1846b20c734261c885f9dc57110871000d9`.
Probe SHA-256:
`0d4cf53f5158f0089d7adf6306842266ec3b64fd050d7af0dc93b4c1c65c7ef7`
(includes complete simulated PHY/SDK/MDIO/Linux stack and component checks).
These identify this debug build, not guaranteed identical builds across paths
or compilers. Preserve upstream MIT notices with any distributed linked binary.

## SDK polling/locking review (2026-11-08)

Reviewed pinned local sources, not live hardware:

- `base/luton26/vtss_luton26.c:22–81`: indirect read/write BUSY loops have no
  count or clock limit, but propagate register-callback errors immediately.
  Direct register access is selected unless `init_conf.spi_bus` is true.
- `base/luton26/vtss_luton26_port.c:160–179,344–365`: SerDes 1G/6G one-shot
  polling is similarly unbounded; calibration BUSY loops at 246–248 and
  483–485 also rely on callback errors. L26_RD/L26_WR macros propagate errors.
- Memory initialization in `vtss_luton26.c:342–351` has at most eleven
  one-millisecond sleeps/reads, followed by an explicit failure if still busy.
  Syscall blocking remains additional to those sleeps.
- Userspace `include/vtss_os_linux.h:126–134` scheduler lock/unlock macros are
  no-ops. The transport mutex protects only one 32-bit window access; it cannot
  serialize an SDK indirect operation's address/data/poll sequence or RMW.
  Production SDK/API operations must share a real outer lock or one owner thread.
  The probe's empty API lock callouts are safe only in this single-thread test.

The probe exercises actual linked SDK indirect read/write functions against
fake callbacks: success after three BUSY polls, failure at each of six read
or five write steps, and permanently BUSY with callback error on transfer nine.
Every error returns without another transfer. This proves callback-error escape,
not elapsed-time bounds or complete coverage of other SDK initialization loops.
SPI mode here is solely test coverage, not a claim that MX84 should enable it.
SDK indirect reads may modify their output with a dummy value before failing;
callers must discard output after any SDK error even though the transport's
own read32 preserves its output on failure. One deadline must cover the whole
SDK operation, with external supervision for blocking syscalls. A future retry
must re-establish/reconcile hardware state rather than resume mid-transaction.

## Separate PHY-library build and discovery hazard (2026-11-08)

The switch-only `libvsc7425.a` is not the copper PHY implementation. The same
pinned public tree carries the MIT MEPA layer and VTSS PHY driver under mepa/.
Built target `mepa_caracal_target` (the public reference composition using
mepa_drv_vtss), without running any board code. This selects a software library
composition, not proof that the reference board matches MX84 wiring or PHYs.
Output `build/mepa/libmepa_caracal.a` SHA-256:
`ca2d5dac92db5d07d1a461e4143ecfd4250591ccab84902eb77f8c3c3ab9c769`.
Keep the upstream mepa/LICENSE and file-level notices with distributed code.

`mepa-offline-probe.c` links that library and supplies only a fake Clause 22
MIIM reader. It calls mepa_phy_id_get, never mepa_create/reset/configuration.
Synthetic IDs, zero and all-one readbacks, and either failed identity half were
checked. Source `mepa/common/src/phy.c` ignores read return codes: a failed
first half of synthetic ID 0x12345678 yields 0x00005678, and a failed second
half yields 0x12340000. The helper continues reading after failure. This test
records a pinned-upstream hazard; it does not bless partial IDs as usable.

Future caller must track callback failures independently of the helper result
and abort discovery on any failure, reject no-device/unsupported identities,
and prevent later probe/reset/configuration I/O after a latched error. A return
value alone cannot establish valid identity. Bound the entire operation and
review mepa_create side effects before allowing live discovery. Actual MX84 PHY
IDs/revisions/required patch paths remain unknown; never infer them from library
availability or synthetic probe values.

The common builder now explicitly builds MEPA and lm_utils, then links the
PHY probe using the utility object library. Initial linking exposed missing
lmu_fmt_czstr/lmu_ss_fmt_last; linking the real utility objects resolved this,
without no-op stubs. Integrated profile, SDK, MDIO, Linux MII and PHY probes pass
on the build host. Log remains `/tmp/mx84-mesa.7aIJrc/probe-build.log`. No live I/O,
daemon query/restart or Fry deployment occurred for this checkpoint.

## Checked PHY identity boundary (2026-11-08)

`phy-identity.c/.h` now provide a narrow Clause 22 identity reader over an
injected MEPA-compatible MIIM callback. It reads register 2 then 3 directly,
checks each return code, stops at the first failure, and commits output only
after both reads succeed. Negative callback errors propagate; unexpected
positive returns become MEPA_RC_ERROR. Null inputs and complete zero/all-one
identities fail. It deliberately avoids the upstream helper's ignored-error
path rather than depending on a global error latch for these two reads.

These are registers of the selected target PHY through MIIM, **not** the
switch-window MDIO selector/data registers. The caller supplies the selected
PHY context, operation serialization and bounded I/O/deadline. This function
does not open hardware, retry, create/reset a PHY, infer a model, or implement
MMD/SPI discovery. A successfully read partial-looking numeric value is not
automatically an I/O error; model/OUI/revision acceptance is a separate gate.

The existing linked MEPA probe now checks our reader too: all error positions,
negative/positive error handling, exact transfer count, invalid inputs, sentinel
rejection, unchanged failed output and valid synthetic values with either zero
half. The full integrated suite passes on the build host. These tests do not prove actual
hardware identity or bounded behavior of a future callback. Read-only Fry check
retained Management connectivity and active mx84-led/mx84-pfsense services; no live changes.

## SDK MIIM path checked offline (2026-11-08)

The switch SDK's public `vtss_miim_read` in vtss_port_api.h accepts instance,
chip, MIIM controller, target-PHY address and register. This bypasses the port
map; therefore a future bridge must derive controller/address from the verified
copper profile and reject CPU/SFP entries, rather than accept arbitrary user
addresses. Bit 7 in the PHY address has special lock-bypass meaning in
`base/ail/vtss_port_api.c:680–706`; never expose that through the MX84 boundary.

`base/luton26/vtss_luton26_port.c:772–830` writes a MIIM controller command
register, polls its BUSY bit, then checks hardware response flags before
extracting the 16-bit read value. These are switch-register transactions through
the existing SDK callouts, not direct host MDIO reads at the target PHY address.
The BUSY loop has no timeout, but L26_RD/WR propagate callback failures.

Extended the linked SDK probe to exercise actual vtss_miim_read using fake
32-bit register callbacks, without initialization or port-map writes. It checks
exact controller-0 command/PHY/register encoding for PHY addresses 0–10 and ID
registers 2/3, success after three status polls, all five transfer failures,
unchanged output after each error, permanently BUSY with error on transfer nine,
and hardware response failure. All pass, along with the existing integrated
suite on the build host. This does not test a live PHY or establish elapsed-time bounds.

The bridge below now wraps both reads in the existing SDK transport ownership
scope. Its caller must still provide one absolute backend deadline and validate
identity before any MEPA device creation/reset. No callback should renew the
deadline. Keep supervised opener/recovery gates separate; do not probe beside
vtss_poca_d.

## Checked identity / SDK bridge (2026-11-08)

`mesa-phy-identity.c/.h` connects checked Clause22 identity reading to actual
vtss_miim_read. It accepts only logical copper 0–10, deriving controller and
PHY address from mx84-board-profile.h. No arbitrary controller/chip/address,
CPU or SFP access is exposed. The function begins one nonblocking operation
scope, checks SDK register callbacks are configured, performs both reads and
ends on success or error. Output is committed after successful reads and
successful scope release. Missing instance/window/output or a busy scope fails
without register I/O. There is no retry, reset, PHY creation, or live opener.

The linked probe exercises bridge -> checked reader -> real SDK MIIM logic
against fake 32-bit register callbacks. For each copper port it tests success
and failure at all ten register transfers across both ID halves, verifies
the ownership scope is held for both commands, checks unchanged failed output,
and confirms cleanup/reuse. Zero/all-one IDs, unconfigured SDK instance, invalid
ports/null inputs and nested operation rejection also pass. Lower MDIO/Linux
layers are tested separately; this is not an end-to-end hardware test.

The bridge borrows a configured SDK instance/window; it does not prove exclusive
physical ownership, validate actual PHY model/revision, or impose a deadline
on an arbitrary callback. The future sole-owner must provide those guarantees
before invocation. Full integrated suite passes on the build host; no Fry deployment or
live PHY discovery. Remaining work: physical identity/patch-path reconstruction,
GPIO/reset roles, and supervised recovery before any hardware trial.

## Complete simulated stack acceptance (2026-11-08)

`test-phy-stack.c`, linked into the software-only SDK probe, now runs the
checked identity bridge -> actual SDK MIIM -> SDK transport callbacks -> MDIO
window -> Linux MII adapter -> injected fake ioctl function. It models switch
address/data selection and the MIIM controller's command/status/data registers,
without opening any socket or calling a real ioctl.

All eleven mapped copper addresses return the synthetic expected identity.
Two BUSY status responses precede completion for each ID half. Each identity
operation uses 56 Linux requests. Failures injected independently at every one
of those requests abort further I/O, preserve output and release operation
ownership. An expired operation deadline yields zero fake requests. Dummy reads
return deliberately wrong 0xaaaa values, confirming they do not reach the ID.
Each fault test starts from fresh simulated controller state; this does not
prove live recovery after partial selector/command writes.

Full integrated checks pass on the build host. ASan/UBSan also pass for all our stack
objects/probe linked against the uninstrumented SDK library. No elapsed-time
bound for an actually blocked syscall or silicon behavior is established.

Existing preserved PuTTY logs were searched for PHY IDs/model/revision; none
were found in those logs. A bounded vendor `help` request returned its supported
commands; no dedicated PHY identity command is exposed there. No debug dump,
raw MDIO, reset, PHY discovery, switch restart or configuration change was run.
Actual PHY identities/revisions and GPIO/reset roles remain hardware gates;
do not select a PHY patch/model from the synthetic fixture data.

## MDIO transaction component (2026-11-08)

`mdio-window.c`/`.h` implement the recovered 32-bit window protocol over injected
16-bit callbacks. There is deliberately no socket/ioctl/device implementation.
The caller must initialize one shared window per physical bus and exclusively
own that bus. A pthread mutex covers the whole address/data transaction, not
just individual transfers. This does not protect against the existing vendor
daemon or a second window/process; it is not permission to run beside either.

Reads select low/high address, issue both dummy reads, then combine low/high
data. Writes select low/high address and write low/high data. Every callback
error aborts the remaining transfers, propagates the error, and releases the
mutex. Failed reads leave the caller's output unchanged. All-one data is valid,
not an error sentinel. Each transaction has a fixed six/four-transfer limit;
backend callbacks still must implement their own bounded waits.

The build script also compiles/runs `test-mdio-window.c`. It uses a fake bus
with only per-transfer locking and scheduling yields to expose transaction
interleaving. Passed on macOS and Linux/GCC; Linux AddressSanitizer plus
UndefinedBehaviorSanitizer also passed:

```sh
cc -std=c11 -Wall -Wextra -Werror -pthread \
  util/mx84/mdio-window.c util/mx84/test-mdio-window.c -o /tmp/mx84-mdio-test
/tmp/mx84-mdio-test
```

Checks cover word/address ordering, dummy-read discard, zero/all-one values,
failure injection at all six read and four write steps, unchanged output after
error, mutex release/reuse, invalid arguments, and 2,000 concurrent mixed
read/write transactions (10,000 individual transfers). No raw switch access
was used. The supervised live opener and hardware acceptance remain pending.

## SDK/MDIO callout adapter (2026-11-08)

`mesa-transport.c/.h` supply callbacks with the SDK's context-free signatures.
The caller begins an operation with an initialized caller-owned MDIO window,
runs the complete SDK operation and ends it on every exit in the same thread.
A global try-lock rejects concurrent or nested operations with EBUSY; a
thread-local binding prevents another thread from invoking callbacks or ending
the owner's operation. Unbound callbacks, nonzero chip numbers and null read
outputs fail without I/O. The lock is never waited on, so lock contention does
not consume an unbounded wait. It does not coordinate other processes.

Callbacks pass register/word addresses unchanged to the MDIO window and map
transport failures to VTSS_RC_ERROR. Read outputs are committed only after a
successful transport read; SDK indirect-read dummy outputs remain the caller's
responsibility as above. The caller must set one backend deadline before begin.
Production callers must not cancel/exit the owner thread while bound; they must
end on error and join callers before destroying the window. No live owner,
socket opener, real ioctl callback, or SDK initialization is supplied here.

The linked probe exercises SDK direct register functions through this adapter
and the MDIO component against fake 16-bit I/O: exact unchanged address halves,
dummy read discard, success, all six/four read/write transfer errors, unchanged
failed-read output, invalid/unbound calls, nested/cross-thread rejection, and
release/reuse after failure. It temporarily substitutes private SDK callbacks,
then restores them, without init_conf_set. Integrated checks pass on the build host.
ASan/UBSan also passed for our probe/adapter/transport objects linked to the
existing uninstrumented SDK library; this is not full SDK sanitizer coverage.

Next is a sole-owner supervised opener and board-specific PHY/SerDes/reset
profile. Keep the live vendor daemon and Management intact until attended
serial recovery, state reconciliation and rollback are available.

## Linux MII ABI adapter (2026-11-08)

`linux-mii.c`/`.h` translate the protocol's 16-bit callbacks into Linux MII
`ifreq` requests. They borrow a descriptor and invoke an injected request
function; they open no socket/file and supply no default real ioctl function.
No executable in this checkpoint can select a live hardware backend.

The interface is fixed to eth0, PHY0, registers0..3. MII data is copied into
the ifreq union inline, as required by the driver's `if_mii()` access, rather
than allocating a buffer through ifr_data. Each request is zero-initialized;
error/invalid return values and changed response identity are rejected. Reads
publish output only after all checks pass. EINTR is propagated without retry.

The caller supplies one absolute CLOCK_MONOTONIC deadline for the encompassing
SDK operation; checks occur before and after each request. Expired operations
fail before further I/O, including a response arriving after its deadline.
**This does not preempt an already blocked syscall.** A production opener must
still enforce sole ownership/serial recovery, review kernel callback bounds,
and provide supervision. A write may have taken effect before a late timeout;
do not interpret the error as rollback or automatically retry it.

`test-linux-mii.c` passed GCC with ASan/UBSan on the build host. Its fake request callback
validates actual Linux header layout, exact ioctl numbers/PHY/interface and
zero padding; an integration test drives the six/four-transfer window through
that ABI. Tests cover kernel-style errors (including unset errno), unexpected
positive returns, changed PHY identity, invalid arguments, deadline expiry
before/after a request, unchanged failed-read output and valid all-one data.
It uses only an inert descriptor number and fake callback. The shared build
helper now runs this check too; actual device access is still untested/gated.

## Board facts recovered without touching hardware

The inspected owner-provided `vtss_poca_d` SHA-256 is
`0a6d330100f281a33a38749e7e4a504ee070d9c39c5c88469b4b8bc72d8a0b46`.
Addresses below identify functions in that binary only; no proprietary code or
binary is included here.

### Register transport

- The daemon uses `eth0`, AF_INET/SOCK_DGRAM, MII ioctls
  SIOCGMIIREG=0x8948 and SIOCSMIIREG=0x8949, PHY address 0.
- 16-bit read/write helpers: 0x474b5e / 0x474bd7.
- 32-bit read helper 0x474c50 selects address low/high via MDIO registers 0/1,
  performs dummy reads of 2/3, then reads 2/3 again as low/high data halves.
- 32-bit write helper 0x474caa selects address through 0/1, writes low data to
  register 2 and high data to register 3.
- The SDK callout wrapper is 0x474d53. SDK Luton26 callouts use register/word
  addresses; its own indirect-access implementation performs the documented
  conversion to VCore byte addresses where needed. Do not add an assumed
  blanket shift in the board adapter.
- Existing MX84 igb patch permits these MII ioctls on the I354 backplane path.

**A register read writes shared address-selection state.** A second process
can race the live vendor daemon even if it promises to read switch registers
only. Do not probe raw MDIO while the daemon owns it. Kernel per-ioctl locking
does not make the whole multi-ioctl transaction atomic. A future adapter must
serialize complete transactions, propagate each ioctl error, and bound waits;
the stock wrapper can conceal low-level errors. The SDK also has busy loops
whose hardware timeout behavior needs review before deployment.

### Port map and indicator path

#### Recovered board profile and ABI warning (2026-11-08)

Further static inspection of the same hash-verified private binary narrows the
board implementation. These are observations, not a ready-to-run initializer:

- Board descriptor constructor `0x409e1d` selects target 0x7425 and installs
  configuration callback `0x409ac5` and board initialization callback `0x409bb7`.
  The configuration callback installs interface callback `0x409ab2`.
  The descriptor name is Pocahontas.
- Configuration callback builds 17 entries at `0x4a9fe0`, stride 0x14, matching
  the chip-port mapping below. Copper entries 0–10 use MIIM controller field 0
  at offset 8 and address byte equal to the logical port at offset 0xc.
  Remaining entries use controller field -1 and address byte 0xff. This
  supports the future PHY-map reconstruction; it does not prove PHY IDs/patches.
- `0x409ab2` returns legacy interface enum 8 for logical 0–10, 10 for 11–16,
  and 0 for >=17. The binary's enum-name function `0x41359d` indexes table
  `0x47b040`: 8 is SGMII and 10 is SERDES. A second CLI table at `0x478f60`
  agrees. Thus copper uses SGMII and SFP/CPU ports use SERDES in this profile.
  SFP physical label/module acceptance remains unverified.
- **Do not copy legacy enum integers into the current SDK.** Pinned public
  `include/vtss/api/types.h` has extra RGMII delay and SGMII variants: its 8 is
  RGMII_TXID and 10 is RTBI. Translate by symbolic interface names. This is a
  concrete ABI mismatch, not an assumption that old struct layouts still match.
- Configuration callback writes numeric 2 at old init-conf offset 0x5c.
  Subsequent tracing identifies this as mux mode: `vtss_init_conf_set` at
  `0x4133e1` copies 0x88 bytes of configuration into SDK state +0x10; stock
  diagnostic `0x414928` reads state +0x6c and prints "Mux mode". A second
  diagnostic at `0x435b6b` reads the same offset and prints "Mux Mode: %u/%u",
  comparing configured mode with the hardware field. This establishes mode 2
  by data flow and independent diagnostics, not offset similarity alone.
  Current SDK `l26_serdes_macro_config` maps VTSS_PORT_MUX_MODE_2 to hardware
  SW_MODE=2. Use that symbolic constant; other struct offsets remain unproved.

`mx84-board-profile.h` now records these verified chip/interface/MIIM facts
and symbolic mux mode as data only. It performs no initialization or access.
The offline linked probe verifies all 17 entries, uniqueness/capacity, MIIM
addresses/sentinels, mux selection and explicit old/current enum separation.
Integrated build checks pass. This table must not be passed to hardware until
the remaining PHY/reset/speed dependencies and recovery gates are satisfied.

#### CPU startup speed and Clause 37 (2026-11-08)

Static data flow further resolves the baseline for CPU logical 13–16:

1. Interface callback returns legacy SERDES (10). Startup jump table at
   `0x4760e0` sends that case to `0x406e71`, setting speed field +0xf8 to 3.
   Stock speed-name table `0x47afe0` identifies 3 as 1G. Common startup then
   sets administrative enable (+0xf4) and duplex (+0xfc) to true and frame
   length (+0x100) to 1518. These are daemon startup defaults, not a complete
   statement about the current live configuration.
2. The per-port configuration block was cleared first (`0x406df6–0x406e1a`).
   Board post-configuration `0x409e88–0x409ed7` changes SERDES autoneg flag
   +0xf5 only for logical 11/12 (true); it leaves CPU 13–16 false. It disables
   front ports administratively but leaves CPU ports enabled.
3. Port apply `0x407700` obtains the SDK port configuration, sets SERDES type,
   power-down from admin enable, speed at stack +0x68 from stored +0xf8,
   duplex at +0x6c from +0xfc, and frame length at +0x80 from +0x100, then
   calls the identified `vtss_port_conf_set` wrapper `0x424a15`.
4. SERDES branch `0x40782c–0x407873` passes stored +0xf5 as enable to
   wrapper `0x424733`, identified by trace string `0x47ede0` as
   `vtss_port_clause_37_control_set`. Thus CPU baseline is forced 1G/full with
   Clause 37 disabled, whereas SFP startup requests Clause 37 enabled.

Translate speed through VTSS_SPEED_1G and set Clause 37 enable explicitly when
eventually implementing the board initializer. Preserve other configuration
fields obtained from SDK defaults until their roles are understood. Do not
apply this finding live: the daemon's later status/control paths can update
speed/configuration, and the working tagged CPU paths need their existing
frame-length/MTU allowance. The stock 1518-byte startup value is not evidence
that the deployed 1516-byte trunk should be reduced. SFP labels/modules and
their signal-detect/negotiation behavior still require physical acceptance.

No running daemon was queried or changed for this trace; it used only the
hash-verified binary's jump/speed tables and disassembly on the build host. CPU peer
configuration, link negotiation and packet/MTU acceptance remain live gates.

Board initialization `0x409bb7–0x409d58` calls independently identified API
wrappers: GPIO-mode-set `0x4203d2`, GPIO-write `0x420591`, and PVLAN-members-set
`0x419589` (confirmed by their embedded API-name trace strings). In order:

| Operation | GPIOs / values |
|---|---|
| Mode numeric 3 | 0, 1, 2 |
| Mode numeric 0, then levels | 3 low; 4 high |
| Mode numeric 0, then levels | 9 low; 11 low |
| Mode numeric 1 | 8, 10, 25, 26 |

This table deliberately retains old numeric mode values; signal roles,
polarity, wiring and safe sequencing remain unproved. No visible delays occur
between those calls in this function; that does not exclude earlier hardware
or SDK reset delays. Do not label GPIO3/4/9/11 as reset or SFP enable yet.

The same callback initializes these logical-port PVLAN groups:

| PVLAN ID | Members | Current role |
|---|---|---|
| 0 | 1, 13 | WAN1 and eth0 CPU |
| 1 | 2, 14 | WAN2 and eth1 CPU |
| 2 | 0, 15 | Management and mgmt0 CPU |
| 3 | 3–12, 16 | LAN/SFP and eth3 CPU |

This independently corroborates why one common CPU trunk could not forward
all front ports under the stock profile. Preserve these paths in initial
replacement trials and keep VLAN isolation as a separate requirement.

Reproduce on the build container without executing the binary:

```sh
objdump -d -M intel --start-address=0x409ab2 --stop-address=0x409d59 PRIVATE_BINARY
objdump -d -M intel --start-address=0x409e1d --stop-address=0x409e85 PRIVATE_BINARY
objdump -d -M intel --start-address=0x41359d --stop-address=0x4135b2 PRIVATE_BINARY
objdump -s --start-address=0x47b040 --stop-address=0x47b0d8 PRIVATE_BINARY
```

Remaining before a board profile can initialize hardware: symbolic interface
translation (offline profile tests now pass), MIIM/PHY identity and required
patches, CPU peer/PCS details beyond the baseline above, GPIO signal roles and
full reset dependencies. The stock binary was only read as a file on the build host;
no service, GPIO, PHY, PVLAN, or raw MDIO operation ran on Fry for this review.

The earlier live vendor port-map readback proves this API-logical -> silicon map:

| API logical | Silicon | Role | Physical label acceptance |
|---|---|---|---|
| 0..10 | 0..10 | MGMT, WAN1/2, LAN1..8 | Copper mapping previously proved |
| 11 | 21 | SFP1 placeholder | Unverified |
| 12 | 20 | SFP2 placeholder | Unverified |
| 13 | 24 | eth0 / WAN1 CPU | Proved |
| 14 | 25 | eth1 / WAN2 CPU | Proved |
| 15 | 23 | mgmt0 / MGMT CPU | Proved; preserve |
| 16 | 22 | eth3 / LAN/SFP CPU | Proved |

The board status callback 0x475000 calls 0x474f25, which calls SDK
`vtss_sgpio_conf_get` (0x420800) and `vtss_sgpio_conf_set` (0x4208dc).
Their embedded API-name strings verify these function identities. A table at
0x490240 maps each front logical port to three SGPIO (slot, bit) pairs:

- Copper logical 0..10 uses matching slot 0..10, bits 0,1,2.
- Logical 11 uses slot 21, bits 0,1,2; logical 12 uses slot 20, bits 0,1,2.
- The callback reacts to link/speed changes, assigning mode values 0/1/6 across
  the three outputs. Which output/color/polarity each wire represents remains
  physically unverified. Do not equate all three outputs with visible LEDs;
  shared SGPIO can also control transceiver/reset signals.

Microchip's public reference board also uses SGPIO for both port LEDs and SFP
transmitter enables. It is a reference, not an MX84 board description. Its
global GPIO/reset/SGPIO initialization must NOT be copied onto Fry blindly.
The installed `mx84-port identify` gate stays closed.

## Next implementation and acceptance

1. Protocol core and Linux ABI/fake tests above are complete. Next connect SDK
   callouts (offline adapter now tested), implement the supervised sole-owner opener/recovery gate, and review
   syscall/SDK busy-loop bounds before creating any live bring-up path.
2. Reconstruct MX84-only SerDes/PHY initialization, reset/clock dependencies,
   per-port interface/speed settings and any required PHY patches using public
   SDK definitions plus the retained baseline. Keep SFP labels unverified.
3. Construct the current three CPU paths and isolated VLAN profile through the
   public APIs. `/etc/mx84/ports.conf` remains authoritative for chassis mapping.
   Preserve MGMT 0/15 and the proved stock PVLAN behavior during initial trials.
4. Own initialization, link polling and SGPIO in ONE process. For identification,
   prove output/color/polarity, then override only a selected port with finite
   duration and restore current hardware link/activity behavior on all exits.
5. Only with attended serial/recovery, compare a full baseline and trial the
   replacement. Require matching switch/Management readback, traffic, isolation,
   MTU, LED restoration and reboot acceptance before replacing the vendor service.

No hardware access, switch initialization, port writes, or LED overrides were
performed for this offline checkpoint. Fry still uses the preserved vendor
runtime; the public SDK build does not prove its replacement will bring up the board.
