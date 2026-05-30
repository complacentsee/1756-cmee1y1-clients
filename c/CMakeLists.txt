# CMakeLists.txt for libbpclient (C) — outbound tag I/O for 1756-CMEE1Y1.
#
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.12)
project(bpclient C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(
    -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes
    -Wno-unused-parameter
    -fPIC
)

include_directories(include)

set(LIB_SOURCES
    src/client.c
    src/tagdb.c
    src/access.c
    src/accessdb.c
    src/errors.c
    src/message.c
    src/struct.c
    src/identity.c
    src/module.c
    src/conn.c
    src/pool.c
    src/route.c
    src/tagcache.c
    src/multitag.c
    src/path.c
    src/wctime.c
)

# Shared library
add_library(bpclient SHARED ${LIB_SOURCES})
target_link_libraries(bpclient PRIVATE pthread rt)
set_target_properties(bpclient PROPERTIES
    PUBLIC_HEADER include/bpclient.h
    SOVERSION 0
    VERSION 0.10.4
)

# Static library (same sources, different target)
add_library(bpclient_static STATIC ${LIB_SOURCES})
set_target_properties(bpclient_static PROPERTIES OUTPUT_NAME bpclient)
target_link_libraries(bpclient_static PRIVATE pthread rt)

# Example: tagtest — canonical smoke test (read/write OCX_TEST)
add_executable(tagtest examples/tagtest.c)
target_link_libraries(tagtest PRIVATE bpclient pthread rt)

# Example: discover — enumerate symbols on a PLC, group by type
add_executable(discover examples/discover.c)
target_link_libraries(discover PRIVATE bpclient pthread rt)

# Example: dumpsym — full symbol-info dump for diagnostic purposes
add_executable(dumpsym examples/dumpsym.c)
target_link_libraries(dumpsym PRIVATE bpclient pthread rt)

# Example: typetest — comprehensive type+array+string round-trip tests
add_executable(typetest examples/typetest.c)
target_link_libraries(typetest PRIVATE bpclient pthread rt m)

# Example: findtags — print all symbols matching a name prefix
add_executable(findtags examples/findtags.c)
target_link_libraries(findtags PRIVATE bpclient pthread rt)

# Example: structprobe — raw-byte dump of OCXcip_GetStructInfo +
# OCXcip_GetStructMbrInfo responses (diagnostic / RE tool)
add_executable(structprobe examples/structprobe.c)
target_link_libraries(structprobe PRIVATE bpclient pthread rt)

# Example: symprobe — raw-byte dump of OCXcip_GetSymbolInfo with an
# oversized payload window (diagnostic / RE tool — used to characterize
# 3-D array layout)
add_executable(symprobe examples/symprobe.c)
target_link_libraries(symprobe PRIVATE bpclient pthread rt)

# Example: testtagver — validate bp_tagdb_test_version() flow
add_executable(testtagver examples/testtagver.c)
target_link_libraries(testtagver PRIVATE bpclient pthread rt)

# Example: msgprobe — manual OCXcip_MessageSend invocation (diagnostic /
# RE tool — used to characterize Phase 3 wire format)
add_executable(msgprobe examples/msgprobe.c)
target_link_libraries(msgprobe PRIVATE bpclient pthread rt)

# Example: identity — query a device's CIP Identity via Get_Attribute_All
add_executable(identity examples/identity.c)
target_link_libraries(identity PRIVATE bpclient pthread rt)

# Example: pathprobe — call OCXcip_ParsePath and dump the canonical
# encoded EPATH bytes (diagnostic / verification tool)
add_executable(pathprobe examples/pathprobe.c)
target_link_libraries(pathprobe PRIVATE bpclient pthread rt)

# Example: actnodes — dump the OCXcip_GetActiveNodeTable bitmap
add_executable(actnodes examples/actnodes.c)
target_link_libraries(actnodes PRIVATE bpclient pthread rt)

# Example: msgprobe_routed — MessageSend with a pre-opened TagDb handle
# (diagnostic: tests whether routing is inherited from CreateTagDbHandle)
add_executable(msgprobe_routed examples/msgprobe_routed.c)
target_link_libraries(msgprobe_routed PRIVATE bpclient pthread rt)

# Example: modutil — local cm1756 module utilities (switch / display / LED)
add_executable(modutil examples/modutil.c)
target_link_libraries(modutil PRIVATE bpclient pthread rt)

# Example: connidentity — Identity via connected (class-3) CIP messaging
add_executable(connidentity examples/connidentity.c)
target_link_libraries(connidentity PRIVATE bpclient pthread rt)

# Example: conntest — v0.7.0 cross-language connected-messaging
# validator (sends N Identities on a class-3 connection; --bench
# does a UCMM-vs-class3 latency micro-benchmark).
add_executable(conntest examples/conntest.c)
target_link_libraries(conntest PRIVATE bpclient pthread rt)

# Example: pooltest — v0.8.0 connection-pool + keepalive validator
# (fans out M workers × N requests through a pre-opened pool; also
# verifies pool_open/close lifecycle and the keepalive thread).
add_executable(pooltest examples/pooltest.c)
target_link_libraries(pooltest PRIVATE bpclient pthread rt)

# Example: routedident — v0.8.0 multi-hop Identity via Unconnected_Send
# (wraps Identity GetAttributesAll in svc 0x52 + route_path).
add_executable(routedident examples/routedident.c)
target_link_libraries(routedident PRIVATE bpclient pthread rt)

# Example: symcache — v0.9.0 symbol-cache validator
# (lookup_symbol cold + warm + after preload_symbols).
add_executable(symcache examples/symcache.c)
target_link_libraries(symcache PRIVATE bpclient pthread rt)

# Example: multitagtest — v0.9.0 read_tags batch validator
# (decodes mixed scalar types in one AccessTagData batch).
add_executable(multitagtest examples/multitagtest.c)
target_link_libraries(multitagtest PRIVATE bpclient pthread rt)

# Example: errstr — v0.10.0 OCXcip_ErrorString validator
# (compares engine-owned strings vs SDK's local bp_strerror).
add_executable(errstr examples/errstr.c)
target_link_libraries(errstr PRIVATE bpclient pthread rt)

# Example: idstatus — v0.10.0 OCXcip_GetDeviceIdStatus validator
# (cross-checks lightweight status against full GetDeviceIdObject).
add_executable(idstatus examples/idstatus.c)
target_link_libraries(idstatus PRIVATE bpclient pthread rt)

# Example: wctime — v0.10.0 GetWCTime/GetWCTimeUTC validator
# (dumps the wall-clock object across slots 1..3).
add_executable(wctime examples/wctime.c)
target_link_libraries(wctime PRIVATE bpclient pthread rt)

# Example: udtinfo — dump a UDT's full schema using the public API
add_executable(udtinfo examples/udtinfo.c)
target_link_libraries(udtinfo PRIVATE bpclient pthread rt)

# Example: dummy — v0.10.3 OCXcip_Dummy liveness-probe validator
add_executable(dummy examples/dummy.c)
target_link_libraries(dummy PRIVATE bpclient pthread rt)

# Example: accessdbtest — v0.10.4 OCXcip_AccessTagDataDb correctness +
# latency validator (cross-checks NEW handle-based path against OLD
# path-string path; reports median + p99 per call for each).
add_executable(accessdbtest examples/accessdbtest.c)
target_link_libraries(accessdbtest PRIVATE bpclient pthread rt)

# Example: structrwtest — atomic whole-UDT read/write validator
# (bp_client_read_struct / bp_client_write_struct, CIP 0x4C/0x4D over
# MessageSend; --write round-trips the just-read bytes byte-identical).
add_executable(structrwtest examples/structrwtest.c)
target_link_libraries(structrwtest PRIVATE bpclient pthread rt)

# Install
include(GNUInstallDirs)
install(TARGETS bpclient
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS bpclient_static
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS
            tagtest
            discover
            dumpsym
            typetest
            findtags
            structprobe
            symprobe
            testtagver
            msgprobe
            msgprobe_routed
            identity
            pathprobe
            actnodes
            modutil
            connidentity
            conntest
            pooltest
            routedident
            symcache
            multitagtest
            errstr
            idstatus
            wctime
            udtinfo
            dummy
            accessdbtest
            structrwtest
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
