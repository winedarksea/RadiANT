/* SPDX-License-Identifier: Apache-2.0
 *
 * Which serial port the USBXpress shim drives: the one the user selected,
 * or none. Linked into DSI_SiUSBXp_3_1.DLL only.
 *
 * The shim forwards every host write to this port, key-setting messages
 * included, so it never guesses. Until 2026-11-24 it took the first CP2102N
 * entry in the registry (present or not) and fell back to COM15, which could
 * hand Zwift's ANT+ network key to whatever else sat on that port. Now it
 * reports no device unless one of these names exactly one port:
 *
 *   RADIANT_ANT_COM=COM11
 *       a port name, COM1..COM999 (any case). Nothing else is accepted, so a
 *       long or odd value cannot overflow a path or point the shim at
 *       another kind of device.
 *   RADIANT_ANT_DEVICE=L1100TY2
 *       a USB serial number: the last part of the instance id of the port's
 *       device or of a USB device above it (an XDS110's ports sit under
 *       USB\VID_0451&PID_BEF3\L1100TY2). Parts Windows made up (they contain
 *       '&') never match.
 *   RADIANT_ANT_DEVICE=USB\VID_0451&PID_BEF3&MI_00\6&...
 *       a device instance id, of the port itself or of a device above it.
 *
 * A device selection must match exactly one PRESENT port; an XDS110 has two
 * (application and auxiliary UART), so its serial number alone is
 * ambiguous, and the log then lists both ports with their instance ids to
 * choose from. With both variables set they must name the same port.
 * scripts/zwift_ant_shim.ps1 -Port / -Device pin them for the user, and
 * -ListDevices prints the candidates.
 */
#include "shim_common.h"
#include <cfgmgr32.h>
#include <string.h>

/* GUID_DEVCLASS_PORTS: "Ports (COM & LPT)". */
static const char ports_class[] = "{4d36e978-e325-11ce-bfc1-08002be10318}";

#define SEL_MSG_MAX 700
#define SEL_LIST_MAX 480
#define DEV_VALUE_MAX MAX_DEVICE_ID_LEN     /* 200 */

int shim_port_name_ok(const char *s)
{
    size_t n = strlen(s), i;

    if (n < 4 || n > 6)
        return 0;
    if ((s[0] | 0x20) != 'c' || (s[1] | 0x20) != 'o' || (s[2] | 0x20) != 'm')
        return 0;
    for (i = 3; i < n; i++)
        if (s[i] < '0' || s[i] > '9')
            return 0;
    return 1;
}

/* What RADIANT_ANT_COM may hold. A test build (SHIM_TEST_PIPES, see
 * test_shim_log.py) also takes pipe\radiant_shim_<word>, so the key
 * redaction and overlapped I/O tests can run on a named pipe; the shim that
 * is installed is never built with it. */
static int com_value_ok(const char *s)
{
#ifdef SHIM_TEST_PIPES
    static const char pre[] = "pipe\\radiant_shim_";
    size_t n = strlen(s), i;

    if (n > sizeof(pre) - 1 && n < SHIM_PORT_MAX
        && strncmp(s, pre, sizeof(pre) - 1) == 0) {
        for (i = sizeof(pre) - 1; i < n; i++)
            if (!(s[i] == '_' || (s[i] >= '0' && s[i] <= '9')
                  || ((s[i] | 0x20) >= 'a' && (s[i] | 0x20) <= 'z')))
                return 0;
        return 1;
    }
#endif
    return shim_port_name_ok(s);
}

/* Printable ASCII with no space, as in a serial number or instance id. */
static int dev_value_ok(const char *s)
{
    size_t n = strlen(s), i;

    if (n == 0 || n > DEV_VALUE_MAX)
        return 0;
    for (i = 0; i < n; i++)
        if (s[i] < 0x21 || s[i] > 0x7E)
            return 0;
    return 1;
}

/* 1 and the value if `name` is set to something that fits, 0 if it is unset
 * or empty, -1 if it is too long to be valid. */
static int env_get(const char *name, char *out, DWORD cap)
{
    DWORD n = GetEnvironmentVariableA(name, out, cap);

    if (n == 0) {
        out[0] = '\0';
        return 0;
    }
    if (n >= cap) {
        out[0] = '\0';
        return -1;
    }
    return 1;
}

/* The last part of a USB instance id, if it is a real serial number. */
static int id_serial_is(const char *id, const char *want)
{
    const char *last;

    if (_strnicmp(id, "USB\\", 4) != 0)
        return 0;
    last = strrchr(id, '\\');
    if (!last || strchr(++last, '&'))
        return 0;                   /* made up by Windows: not a serial */
    return _stricmp(last, want) == 0;
}

/* Whether the device `dn` (instance id `id`) or a device above it is the
 * one `want` names. */
static int dev_matches(DEVINST dn, const char *id, const char *want,
                       int by_id)
{
    char pid[MAX_DEVICE_ID_LEN + 1];
    const char *cur = id;
    DEVINST parent;
    int depth;

    for (depth = 0; depth < 16; depth++) {
        if (by_id ? _stricmp(cur, want) == 0 : id_serial_is(cur, want))
            return 1;
        if (CM_Get_Parent(&parent, dn, 0) != CR_SUCCESS
            || CM_Get_Device_IDA(parent, pid, sizeof(pid), 0) != CR_SUCCESS)
            return 0;
        dn = parent;
        cur = pid;
    }
    return 0;
}

/* The PortName of a port device, if it is one the shim may open. */
static int dev_port_name(DEVINST dn, char *out, DWORD cap)
{
    HKEY hk;
    DWORD type, cb = cap - 1;
    LONG rc;

    out[0] = '\0';
    if (CM_Open_DevNode_Key(dn, KEY_READ, 0, RegDisposition_OpenExisting,
                            &hk, CM_REGISTRY_HARDWARE) != CR_SUCCESS)
        return 0;
    rc = RegQueryValueExA(hk, "PortName", NULL, &type, (LPBYTE)out, &cb);
    RegCloseKey(hk);
    if (rc != ERROR_SUCCESS || type != REG_SZ) {
        out[0] = '\0';
        return 0;
    }
    out[cb] = '\0';                 /* REG_SZ need not be terminated */
    return shim_port_name_ok(out);
}

/* How many present COM ports `want` names. The first one's name goes in
 * `out`; each match is added to `list` as "COM11 <instance id>" for the
 * log. */
static int resolve_device(const char *want, char *out, size_t cap,
                          char *list, size_t lcap)
{
    const ULONG flags = CM_GETIDLIST_FILTER_CLASS | CM_GETIDLIST_FILTER_PRESENT;
    int by_id = strchr(want, '\\') != NULL, n = 0, tries;
    char *ids = NULL, *id, port[16];
    size_t lo = 0;
    ULONG len = 0;
    CONFIGRET cr = CR_FAILURE;
    DEVINST dn;

    out[0] = '\0';
    list[0] = '\0';
    /* The list can grow between the size call and the fetch. */
    for (tries = 0; tries < 3; tries++) {
        if (CM_Get_Device_ID_List_SizeA(&len, ports_class, flags)
                != CR_SUCCESS || len < 2)
            return 0;
        ids = (char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, len);
        if (!ids)
            return 0;
        cr = CM_Get_Device_ID_ListA(ports_class, ids, len, flags);
        if (cr == CR_SUCCESS)
            break;
        HeapFree(GetProcessHeap(), 0, ids);
        ids = NULL;
        if (cr != CR_BUFFER_SMALL)
            return 0;
    }
    if (!ids)
        return 0;

    for (id = ids; *id; id += strlen(id) + 1) {
        if (CM_Locate_DevNodeA(&dn, id, CM_LOCATE_DEVNODE_NORMAL)
                != CR_SUCCESS
            || !dev_matches(dn, id, want, by_id)
            || !dev_port_name(dn, port, sizeof(port)))
            continue;
        if (n++ == 0)
            strcpy_s(out, cap, port);
        if (lo < lcap) {
            int w = _snprintf_s(list + lo, lcap - lo, _TRUNCATE, "%s%s %s",
                                lo ? "; " : "", port, id);
            lo = w < 0 ? lcap : lo + (size_t)w;
        }
    }
    HeapFree(GetProcessHeap(), 0, ids);
    return n;
}

static void upcase(char *s)
{
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z')
            *s = (char)(*s - 'a' + 'A');
}

static SRWLOCK g_sel_lock = SRWLOCK_INIT;
static char g_sel_last[SEL_MSG_MAX];

/* Logs `msg` unless the previous selection logged the same: SI_GetNumDevices
 * resolves on every call. */
static void sel_log(const char *msg)
{
    int changed;

    AcquireSRWLockExclusive(&g_sel_lock);
    changed = strcmp(g_sel_last, msg) != 0;
    if (changed)
        strncpy_s(g_sel_last, sizeof(g_sel_last), msg, _TRUNCATE);
    ReleaseSRWLockExclusive(&g_sel_lock);
    if (changed)
        shim_log("%s", msg);
}

int shim_select_port(char *out, size_t cap)
{
    char com[SHIM_PORT_MAX], dev[DEV_VALUE_MAX + 1], found[16];
    char list[SEL_LIST_MAX], msg[SEL_MSG_MAX];
    int hc = env_get("RADIANT_ANT_COM", com, sizeof(com));
    int hd = env_get("RADIANT_ANT_DEVICE", dev, sizeof(dev));
    int n, ok = 0;

    out[0] = '\0';
    if (hc < 0 || (hc > 0 && !com_value_ok(com))) {
        /* Not echoed: it is not a port name, and could be anything. */
        strcpy_s(msg, sizeof(msg), "no port: RADIANT_ANT_COM is set but is not"
                 " a port name (COM1..COM999)");
    } else if (hd < 0 || (hd > 0 && !dev_value_ok(dev))) {
        strcpy_s(msg, sizeof(msg), "no port: RADIANT_ANT_DEVICE is set but is"
                 " not a serial number or device instance id");
    } else if (hd > 0) {
        if (hc > 0)
            upcase(com);
        n = resolve_device(dev, found, sizeof(found), list, sizeof(list));
        if (n == 0)
            _snprintf_s(msg, sizeof(msg), _TRUNCATE, "no port: RADIANT_ANT_DEVICE"
                        "=%s names no present COM port", dev);
        else if (n > 1)
            _snprintf_s(msg, sizeof(msg), _TRUNCATE, "no port: RADIANT_ANT_DEVICE"
                        "=%s is ambiguous, it names %d COM ports (%s); set it"
                        " to one port's instance id", dev, n, list);
        else if (hc > 0 && _stricmp(found, com) != 0)
            _snprintf_s(msg, sizeof(msg), _TRUNCATE, "no port: RADIANT_ANT_COM"
                        "=%s and RADIANT_ANT_DEVICE=%s (%s) disagree", com, dev,
                        list);
        else {
            strcpy_s(out, cap, found);
            _snprintf_s(msg, sizeof(msg), _TRUNCATE, "port %s (from"
                        " RADIANT_ANT_DEVICE=%s: %s%s)", found, dev, list,
                        hc > 0 ? ", agreeing with RADIANT_ANT_COM" : "");
            ok = 1;
        }
    } else if (hc > 0) {
        if (shim_port_name_ok(com))
            upcase(com);            /* not a test pipe's name */
        strcpy_s(out, cap, com);
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "port %s (from RADIANT_ANT_COM)",
                    com);
        ok = 1;
    } else {
        strcpy_s(msg, sizeof(msg), "no port: none selected (set RADIANT_ANT_COM"
                 " or RADIANT_ANT_DEVICE; scripts/zwift_ant_shim.ps1 -Port,"
                 " -Device, -ListDevices)");
    }
    sel_log(msg);
    return ok;
}
