/*
 * qemu_capabilities.c: QEMU capabilities generation
 *
 * Copyright (C) 2006-2013 Red Hat, Inc.
 * Copyright (C) 2006 Daniel P. Berrange
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */

#include <config.h>

#include "qemu_capabilities.h"
#include "viralloc.h"
#include "virlog.h"
#include "virerror.h"
#include "virutil.h"
#include "virfile.h"
#include "virpidfile.h"
#include "virprocess.h"
#include "nodeinfo.h"
#include "cpu/cpu.h"
#include "domain_conf.h"
#include "vircommand.h"
#include "virbitmap.h"
#include "virnodesuspend.h"
#include "qemu_monitor.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdarg.h>

#define VIR_FROM_THIS VIR_FROM_QEMU

/* While not public, these strings must not change. They
 * are used in domain status files which are read on
 * daemon restarts
 */
VIR_ENUM_IMPL(virQEMUCaps, QEMU_CAPS_LAST,
              "kqemu",  /* 0 */
              "vnc-colon",
              "no-reboot",
              "drive",
              "drive-boot",

              "name", /* 5 */
              "uuid",
              "domid",
              "vnet-hdr",
              "migrate-kvm-stdio",

              "migrate-qemu-tcp", /* 10 */
              "migrate-qemu-exec",
              "drive-cache-v2",
              "kvm",
              "drive-format",

              "vga", /* 15 */
              "0.10",
              "pci-device",
              "mem-path",
              "drive-serial",

              "xen-domid", /* 20 */
              "migrate-qemu-unix",
              "chardev",
              "enable-kvm",
              "monitor-json",

              "balloon", /* 25 */
              "device",
              "sdl",
              "smp-topology",
              "netdev",

              "rtc", /* 30 */
              "vhost-net",
              "rtc-td-hack",
              "no-hpet",
              "no-kvm-pit",

              "tdf", /* 35 */
              "pci-configfd",
              "nodefconfig",
              "boot-menu",
              "enable-kqemu",

              "fsdev", /* 40 */
              "nesting",
              "name-process",
              "drive-readonly",
              "smbios-type",

              "vga-qxl", /* 45 */
              "spice",
              "vga-none",
              "migrate-qemu-fd",
              "boot-index",

              "hda-duplex", /* 50 */
              "drive-aio",
              "pci-multibus",
              "pci-bootindex",
              "ccid-emulated",

              "ccid-passthru", /* 55 */
              "chardev-spicevmc",
              "device-spicevmc",
              "virtio-tx-alg",
              "device-qxl-vga",

              "pci-multifunction", /* 60 */
              "virtio-blk-pci.ioeventfd",
              "sga",
              "virtio-blk-pci.event_idx",
              "virtio-net-pci.event_idx",

              "cache-directsync", /* 65 */
              "piix3-usb-uhci",
              "piix4-usb-uhci",
              "usb-ehci",
              "ich9-usb-ehci1",

              "vt82c686b-usb-uhci", /* 70 */
              "pci-ohci",
              "usb-redir",
              "usb-hub",
              "no-shutdown",

              "cache-unsafe", /* 75 */
              "rombar",
              "ich9-ahci",
              "no-acpi",
              "fsdev-readonly",

              "virtio-blk-pci.scsi", /* 80 */
              "blk-sg-io",
              "drive-copy-on-read",
              "cpu-host",
              "fsdev-writeout",

              "drive-iotune", /* 85 */
              "system_wakeup",
              "scsi-disk.channel",
              "scsi-block",
              "transaction",

              "block-job-sync", /* 90 */
              "block-job-async",
              "scsi-cd",
              "ide-cd",
              "no-user-config",

              "hda-micro", /* 95 */
              "dump-guest-memory",
              "nec-usb-xhci",
              "virtio-s390",
              "balloon-event",

              "bridge", /* 100 */
              "lsi",
              "virtio-scsi-pci",
              "blockio",
              "disable-s3",

              "disable-s4", /* 105 */
              "usb-redir.filter",
              "ide-drive.wwn",
              "scsi-disk.wwn",
              "seccomp-sandbox",

              "reboot-timeout", /* 110 */
              "dump-guest-core",
              "seamless-migration",
              "block-commit",
              "vnc",

              "drive-mirror", /* 115 */
              "usb-redir.bootindex",
              "usb-host.bootindex",
              "blockdev-snapshot-sync",
              "qxl",

              "VGA", /* 120 */
              "cirrus-vga",
              "vmware-svga",
              "device-video-primary",
              "s390-sclp",

              "usb-serial", /* 125 */
              "usb-net",
              "add-fd",
              "nbd-server",
              "virtio-rng",

              "rng-random", /* 130 */
              "rng-egd",
    );

struct _virQEMUCaps {
    virObject object;

    bool usedQMP;

    char *binary;
    time_t mtime;

    virBitmapPtr flags;

    unsigned int version;
    unsigned int kvmVersion;

    virArch arch;

    size_t ncpuDefinitions;
    char **cpuDefinitions;

    size_t nmachineTypes;
    char **machineTypes;
    char **machineAliases;
};

struct _virQEMUCapsCache {
    virMutex lock;
    virHashTablePtr binaries;
    char *libDir;
    char *runDir;
    uid_t runUid;
    gid_t runGid;
};


static virClassPtr virQEMUCapsClass;
static void virQEMUCapsDispose(void *obj);

static int virQEMUCapsOnceInit(void)
{
    if (!(virQEMUCapsClass = virClassNew(virClassForObject(),
                                         "virQEMUCaps",
                                         sizeof(virQEMUCaps),
                                         virQEMUCapsDispose)))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(virQEMUCaps)

static virArch virQEMUCapsArchFromString(const char *arch)
{
    if (STREQ(arch, "i386"))
        return VIR_ARCH_I686;
    if (STREQ(arch, "arm"))
        return VIR_ARCH_ARMV7L;

    return virArchFromString(arch);
}


static const char *virQEMUCapsArchToString(virArch arch)
{
    if (arch == VIR_ARCH_I686)
        return "i386";
    else if (arch == VIR_ARCH_ARMV7L)
        return "arm";

    return virArchToString(arch);
}


static virCommandPtr
virQEMUCapsProbeCommand(const char *qemu,
                        virQEMUCapsPtr qemuCaps,
                        uid_t runUid, gid_t runGid)
{
    virCommandPtr cmd = virCommandNew(qemu);

    if (qemuCaps) {
        if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_NO_USER_CONFIG))
            virCommandAddArg(cmd, "-no-user-config");
        else if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_NODEFCONFIG))
            virCommandAddArg(cmd, "-nodefconfig");
    }

    virCommandAddEnvPassCommon(cmd);
    virCommandClearCaps(cmd);
    virCommandSetGID(cmd, runGid);
    virCommandSetUID(cmd, runUid);

    return cmd;
}


static void
virQEMUCapsSetDefaultMachine(virQEMUCapsPtr qemuCaps,
                             size_t defIdx)
{
    char *name = qemuCaps->machineTypes[defIdx];
    char *alias = qemuCaps->machineAliases[defIdx];

    memmove(qemuCaps->machineTypes + 1,
            qemuCaps->machineTypes,
            sizeof(qemuCaps->machineTypes[0]) * defIdx);
    memmove(qemuCaps->machineAliases + 1,
            qemuCaps->machineAliases,
            sizeof(qemuCaps->machineAliases[0]) * defIdx);
    qemuCaps->machineTypes[0] = name;
    qemuCaps->machineAliases[0] = alias;
}

/* Format is:
 * <machine> <desc> [(default)|(alias of <canonical>)]
 */
static int
virQEMUCapsParseMachineTypesStr(const char *output,
                                virQEMUCapsPtr qemuCaps)
{
    const char *p = output;
    const char *next;
    size_t defIdx = 0;

    do {
        const char *t;
        char *name;
        char *canonical = NULL;

        if ((next = strchr(p, '\n')))
            ++next;

        if (STRPREFIX(p, "Supported machines are:"))
            continue;

        if (!(t = strchr(p, ' ')) || (next && t >= next))
            continue;

        if (!(name = strndup(p, t - p)))
            goto no_memory;

        p = t;
        if ((t = strstr(p, "(default)")) && (!next || t < next))
            defIdx = qemuCaps->nmachineTypes;

        if ((t = strstr(p, "(alias of ")) && (!next || t < next)) {
            p = t + strlen("(alias of ");
            if (!(t = strchr(p, ')')) || (next && t >= next))
                continue;

            if (!(canonical = strndup(p, t - p))) {
                VIR_FREE(name);
                goto no_memory;
            }
        }

        if (VIR_REALLOC_N(qemuCaps->machineTypes, qemuCaps->nmachineTypes + 1) < 0 ||
            VIR_REALLOC_N(qemuCaps->machineAliases, qemuCaps->nmachineTypes + 1) < 0) {
            VIR_FREE(name);
            VIR_FREE(canonical);
            goto no_memory;
        }
        qemuCaps->nmachineTypes++;
        if (canonical) {
            qemuCaps->machineTypes[qemuCaps->nmachineTypes-1] = canonical;
            qemuCaps->machineAliases[qemuCaps->nmachineTypes-1] = name;
        } else {
            qemuCaps->machineTypes[qemuCaps->nmachineTypes-1] = name;
            qemuCaps->machineAliases[qemuCaps->nmachineTypes-1] = NULL;
        }
    } while ((p = next));


    if (defIdx)
        virQEMUCapsSetDefaultMachine(qemuCaps, defIdx);

    return 0;

no_memory:
    virReportOOMError();
    return -1;
}

static int
virQEMUCapsProbeMachineTypes(virQEMUCapsPtr qemuCaps,
                             uid_t runUid, gid_t runGid)
{
    char *output;
    int ret = -1;
    virCommandPtr cmd;
    int status;

    /* Make sure the binary we are about to try exec'ing exists.
     * Technically we could catch the exec() failure, but that's
     * in a sub-process so it's hard to feed back a useful error.
     */
    if (!virFileIsExecutable(qemuCaps->binary)) {
        virReportSystemError(errno, _("Cannot find QEMU binary %s"),
                             qemuCaps->binary);
        return -1;
    }

    cmd = virQEMUCapsProbeCommand(qemuCaps->binary, qemuCaps, runUid, runGid);
    virCommandAddArgList(cmd, "-M", "?", NULL);
    virCommandSetOutputBuffer(cmd, &output);

    /* Ignore failure from older qemu that did not understand '-M ?'.  */
    if (virCommandRun(cmd, &status) < 0)
        goto cleanup;

    if (virQEMUCapsParseMachineTypesStr(output, qemuCaps) < 0)
        goto cleanup;

    ret = 0;

cleanup:
    VIR_FREE(output);
    virCommandFree(cmd);

    return ret;
}


typedef int
(*virQEMUCapsParseCPUModels)(const char *output,
                             virQEMUCapsPtr qemuCaps);

/* Format:
 *      <arch> <model>
 * qemu-0.13 encloses some model names in []:
 *      <arch> [<model>]
 */
static int
virQEMUCapsParseX86Models(const char *output,
                          virQEMUCapsPtr qemuCaps)
{
    const char *p = output;
    const char *next;
    int ret = -1;

    do {
        const char *t;
        size_t len;

        if ((next = strchr(p, '\n')))
            next++;

        if (!(t = strchr(p, ' ')) || (next && t >= next))
            continue;

        if (!STRPREFIX(p, "x86"))
            continue;

        p = t;
        while (*p == ' ')
            p++;

        if (*p == '\0' || *p == '\n')
            continue;

        if (VIR_EXPAND_N(qemuCaps->cpuDefinitions, qemuCaps->ncpuDefinitions, 1) < 0) {
            virReportOOMError();
            goto cleanup;
        }

        if (next)
            len = next - p - 1;
        else
            len = strlen(p);

        if (len > 2 && *p == '[' && p[len - 1] == ']') {
            p++;
            len -= 2;
        }

        if (!(qemuCaps->cpuDefinitions[qemuCaps->ncpuDefinitions - 1] = strndup(p, len))) {
            virReportOOMError();
            goto cleanup;
        }
    } while ((p = next));

    ret = 0;

cleanup:
    return ret;
}

/* ppc64 parser.
 * Format : PowerPC <machine> <description>
 */
static int
virQEMUCapsParsePPCModels(const char *output,
                          virQEMUCapsPtr qemuCaps)
{
    const char *p = output;
    const char *next;
    int ret = -1;

    do {
        const char *t;
        size_t len;

        if ((next = strchr(p, '\n')))
            next++;

        if (!STRPREFIX(p, "PowerPC "))
            continue;

        /* Skip the preceding sub-string "PowerPC " */
        p += 8;

        /*Malformed string, does not obey the format 'PowerPC <model> <desc>'*/
        if (!(t = strchr(p, ' ')) || (next && t >= next))
            continue;

        if (*p == '\0')
            break;

        if (*p == '\n')
            continue;

        if (VIR_EXPAND_N(qemuCaps->cpuDefinitions, qemuCaps->ncpuDefinitions, 1) < 0) {
            virReportOOMError();
            goto cleanup;
        }

        len = t - p - 1;

        if (!(qemuCaps->cpuDefinitions[qemuCaps->ncpuDefinitions - 1] = strndup(p, len))) {
            virReportOOMError();
            goto cleanup;
        }
    } while ((p = next));

    ret = 0;

cleanup:
    return ret;
}

static int
virQEMUCapsProbeCPUModels(virQEMUCapsPtr qemuCaps, uid_t runUid, gid_t runGid)
{
    char *output = NULL;
    int ret = -1;
    virQEMUCapsParseCPUModels parse;
    virCommandPtr cmd;

    if (qemuCaps->arch == VIR_ARCH_I686 ||
        qemuCaps->arch == VIR_ARCH_X86_64)
        parse = virQEMUCapsParseX86Models;
    else if (qemuCaps->arch == VIR_ARCH_PPC64)
        parse = virQEMUCapsParsePPCModels;
    else {
        VIR_DEBUG("don't know how to parse %s CPU models",
                  virArchToString(qemuCaps->arch));
        return 0;
    }

    cmd = virQEMUCapsProbeCommand(qemuCaps->binary, qemuCaps, runUid, runGid);
    virCommandAddArgList(cmd, "-cpu", "?", NULL);
    virCommandSetOutputBuffer(cmd, &output);

    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;

    if (parse(output, qemuCaps) < 0)
        goto cleanup;

    ret = 0;

cleanup:
    VIR_FREE(output);
    virCommandFree(cmd);

    return ret;
}


static char *
virQEMUCapsFindBinaryForArch(virArch hostarch,
                             virArch guestarch)
{
    char *ret;
    const char *archstr = virQEMUCapsArchToString(guestarch);
    char *binary;

    if (virAsprintf(&binary, "qemu-system-%s", archstr) < 0) {
        virReportOOMError();
        return NULL;
    }

    ret = virFindFileInPath(binary);
    VIR_FREE(binary);
    if (ret && !virFileIsExecutable(ret))
        VIR_FREE(ret);

    if (guestarch == VIR_ARCH_I686 &&
        !ret &&
        hostarch == VIR_ARCH_X86_64) {
        ret = virFindFileInPath("qemu-system-x86_64");
        if (ret && !virFileIsExecutable(ret))
            VIR_FREE(ret);
    }

    if (guestarch == VIR_ARCH_I686 &&
        !ret) {
        ret = virFindFileInPath("qemu");
        if (ret && !virFileIsExecutable(ret))
            VIR_FREE(ret);
    }

    return ret;
}


static bool
virQEMUCapsIsValidForKVM(virArch hostarch,
                         virArch guestarch)
{
    if (hostarch == guestarch)
        return true;
    if (hostarch == VIR_ARCH_X86_64 &&
        guestarch == VIR_ARCH_I686)
        return true;
    return false;
}

static int
virQEMUCapsInitGuest(virCapsPtr caps,
                     virQEMUCapsCachePtr cache,
                     virArch hostarch,
                     virArch guestarch)
{
    virCapsGuestPtr guest;
    int i;
    int haskvm = 0;
    int haskqemu = 0;
    char *kvmbin = NULL;
    char *binary = NULL;
    virCapsGuestMachinePtr *machines = NULL;
    size_t nmachines = 0;
    virQEMUCapsPtr qemubinCaps = NULL;
    virQEMUCapsPtr kvmbinCaps = NULL;
    int ret = -1;

    /* Check for existence of base emulator, or alternate base
     * which can be used with magic cpu choice
     */
    binary = virQEMUCapsFindBinaryForArch(hostarch, guestarch);

    /* Ignore binary if extracting version info fails */
    if (binary) {
        if (!(qemubinCaps = virQEMUCapsCacheLookup(cache, binary))) {
            virResetLastError();
            VIR_FREE(binary);
        }
    }

    /* qemu-kvm/kvm binaries can only be used if
     *  - host & guest arches match
     * Or
     *  - hostarch is x86_64 and guest arch is i686
     * The latter simply needs "-cpu qemu32"
     */
    if (virQEMUCapsIsValidForKVM(hostarch, guestarch)) {
        const char *const kvmbins[] = { "/usr/libexec/qemu-kvm", /* RHEL */
                                        "qemu-kvm", /* Fedora */
                                        "kvm" }; /* Upstream .spec */

        for (i = 0; i < ARRAY_CARDINALITY(kvmbins); ++i) {
            kvmbin = virFindFileInPath(kvmbins[i]);

            if (!kvmbin)
                continue;

            if (!(kvmbinCaps = virQEMUCapsCacheLookup(cache, kvmbin))) {
                virResetLastError();
                VIR_FREE(kvmbin);
                continue;
            }

            if (!binary) {
                binary = kvmbin;
                qemubinCaps = kvmbinCaps;
                kvmbin = NULL;
                kvmbinCaps = NULL;
            }
            break;
        }
    }

    if (!binary)
        return 0;

    if (access("/dev/kvm", F_OK) == 0 &&
        (virQEMUCapsGet(qemubinCaps, QEMU_CAPS_KVM) ||
         virQEMUCapsGet(qemubinCaps, QEMU_CAPS_ENABLE_KVM) ||
         kvmbin))
        haskvm = 1;

    if (access("/dev/kqemu", F_OK) == 0 &&
        virQEMUCapsGet(qemubinCaps, QEMU_CAPS_KQEMU))
        haskqemu = 1;

    if (virQEMUCapsGetMachineTypesCaps(qemubinCaps, &nmachines, &machines) < 0)
        goto error;

    /* We register kvm as the base emulator too, since we can
     * just give -no-kvm to disable acceleration if required */
    if ((guest = virCapabilitiesAddGuest(caps,
                                         "hvm",
                                         guestarch,
                                         binary,
                                         NULL,
                                         nmachines,
                                         machines)) == NULL)
        goto error;

    machines = NULL;
    nmachines = 0;

    if (caps->host.cpu &&
        caps->host.cpu->model &&
        virQEMUCapsGetCPUDefinitions(qemubinCaps, NULL) > 0 &&
        !virCapabilitiesAddGuestFeature(guest, "cpuselection", 1, 0))
        goto error;

    if (virQEMUCapsGet(qemubinCaps, QEMU_CAPS_BOOTINDEX) &&
        !virCapabilitiesAddGuestFeature(guest, "deviceboot", 1, 0))
        goto error;

    if (virCapabilitiesAddGuestDomain(guest,
                                      "qemu",
                                      NULL,
                                      NULL,
                                      0,
                                      NULL) == NULL)
        goto error;

    if (haskqemu &&
        virCapabilitiesAddGuestDomain(guest,
                                      "kqemu",
                                      NULL,
                                      NULL,
                                      0,
                                      NULL) == NULL)
        goto error;

    if (haskvm) {
        virCapsGuestDomainPtr dom;

        if (kvmbin &&
            virQEMUCapsGetMachineTypesCaps(kvmbinCaps, &nmachines, &machines) < 0)
            goto error;

        if ((dom = virCapabilitiesAddGuestDomain(guest,
                                                 "kvm",
                                                 kvmbin ? kvmbin : binary,
                                                 NULL,
                                                 nmachines,
                                                 machines)) == NULL) {
            goto error;
        }

        machines = NULL;
        nmachines = 0;

    }

    if (((guestarch == VIR_ARCH_I686) ||
         (guestarch == VIR_ARCH_X86_64)) &&
        (virCapabilitiesAddGuestFeature(guest, "acpi", 1, 1) == NULL ||
         virCapabilitiesAddGuestFeature(guest, "apic", 1, 0) == NULL))
        goto error;

    if ((guestarch == VIR_ARCH_I686) &&
        (virCapabilitiesAddGuestFeature(guest, "pae", 1, 0) == NULL ||
         virCapabilitiesAddGuestFeature(guest, "nonpae", 1, 0) == NULL))
        goto error;

    ret = 0;

cleanup:
    VIR_FREE(binary);
    VIR_FREE(kvmbin);
    virObjectUnref(qemubinCaps);
    virObjectUnref(kvmbinCaps);

    return ret;

error:
    virCapabilitiesFreeMachines(machines, nmachines);

    goto cleanup;
}


static int
virQEMUCapsInitCPU(virCapsPtr caps,
                   virArch arch)
{
    virCPUDefPtr cpu = NULL;
    union cpuData *data = NULL;
    virNodeInfo nodeinfo;
    int ret = -1;

    if (VIR_ALLOC(cpu) < 0) {
        virReportOOMError();
        goto error;
    }

    cpu->arch = arch;

    if (nodeGetInfo(NULL, &nodeinfo))
        goto error;

    cpu->type = VIR_CPU_TYPE_HOST;
    cpu->sockets = nodeinfo.sockets;
    cpu->cores = nodeinfo.cores;
    cpu->threads = nodeinfo.threads;
    caps->host.cpu = cpu;

    if (!(data = cpuNodeData(arch))
        || cpuDecode(cpu, data, NULL, 0, NULL) < 0)
        goto cleanup;

    ret = 0;

cleanup:
    cpuDataFree(arch, data);

    return ret;

error:
    virCPUDefFree(cpu);
    goto cleanup;
}


static int virQEMUCapsDefaultConsoleType(const char *ostype ATTRIBUTE_UNUSED,
                                         virArch arch)
{
    if (arch == VIR_ARCH_S390 ||
        arch == VIR_ARCH_S390X)
        return VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_VIRTIO;
    else
        return VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL;
}


virCapsPtr virQEMUCapsInit(virQEMUCapsCachePtr cache)
{
    virCapsPtr caps;
    int i;

    if ((caps = virCapabilitiesNew(virArchFromHost(),
                                   1, 1)) == NULL)
        goto error;

    /* Using KVM's mac prefix for QEMU too */
    virCapabilitiesSetMacPrefix(caps, (unsigned char[]){ 0x52, 0x54, 0x00 });

    /* Some machines have problematic NUMA toplogy causing
     * unexpected failures. We don't want to break the QEMU
     * driver in this scenario, so log errors & carry on
     */
    if (nodeCapsInitNUMA(caps) < 0) {
        virCapabilitiesFreeNUMAInfo(caps);
        VIR_WARN("Failed to query host NUMA topology, disabling NUMA capabilities");
    }

    if (virQEMUCapsInitCPU(caps, virArchFromHost()) < 0)
        VIR_WARN("Failed to get host CPU");

    /* Add the power management features of the host */

    if (virNodeSuspendGetTargetMask(&caps->host.powerMgmt) < 0)
        VIR_WARN("Failed to get host power management capabilities");

    virCapabilitiesAddHostMigrateTransport(caps,
                                           "tcp");

    /* QEMU can support pretty much every arch that exists,
     * so just probe for them all - we gracefully fail
     * if a qemu-system-$ARCH binary can't be found
     */
    for (i = 0 ; i < VIR_ARCH_LAST ; i++)
        if (virQEMUCapsInitGuest(caps, cache,
                                 virArchFromHost(),
                                 i) < 0)
            goto error;

    /* QEMU Requires an emulator in the XML */
    virCapabilitiesSetEmulatorRequired(caps);

    caps->defaultConsoleTargetType = virQEMUCapsDefaultConsoleType;

    return caps;

error:
    virObjectUnref(caps);
    return NULL;
}


static int
virQEMUCapsComputeCmdFlags(const char *help,
                           unsigned int version,
                           unsigned int is_kvm,
                           unsigned int kvm_version,
                           virQEMUCapsPtr qemuCaps,
                           bool check_yajl ATTRIBUTE_UNUSED)
{
    const char *p;
    const char *fsdev, *netdev;

    if (strstr(help, "-no-kqemu"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_KQEMU);
    if (strstr(help, "-enable-kqemu"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_ENABLE_KQEMU);
    if (strstr(help, "-no-kvm"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_KVM);
    if (strstr(help, "-enable-kvm"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_ENABLE_KVM);
    if (strstr(help, "-no-reboot"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NO_REBOOT);
    if (strstr(help, "-name")) {
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NAME);
        if (strstr(help, ",process="))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_NAME_PROCESS);
    }
    if (strstr(help, "-uuid"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_UUID);
    if (strstr(help, "-xen-domid"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_XEN_DOMID);
    else if (strstr(help, "-domid"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_DOMID);
    if (strstr(help, "-drive")) {
        const char *cache = strstr(help, "cache=");

        virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE);
        if (cache && (p = strchr(cache, ']'))) {
            if (memmem(cache, p - cache, "on|off", sizeof("on|off") - 1) == NULL)
                virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_CACHE_V2);
            if (memmem(cache, p - cache, "directsync", sizeof("directsync") - 1))
                virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_CACHE_DIRECTSYNC);
            if (memmem(cache, p - cache, "unsafe", sizeof("unsafe") - 1))
                virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_CACHE_UNSAFE);
        }
        if (strstr(help, "format="))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_FORMAT);
        if (strstr(help, "readonly="))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_READONLY);
        if (strstr(help, "aio=threads|native"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_AIO);
        if (strstr(help, "copy-on-read=on|off"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_COPY_ON_READ);
        if (strstr(help, "bps="))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_IOTUNE);
    }
    if ((p = strstr(help, "-vga")) && !strstr(help, "-std-vga")) {
        const char *nl = strstr(p, "\n");

        virQEMUCapsSet(qemuCaps, QEMU_CAPS_VGA);

        if (strstr(p, "|qxl"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_VGA_QXL);
        if ((p = strstr(p, "|none")) && p < nl)
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_VGA_NONE);
    }
    if (strstr(help, "-spice"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_SPICE);
    if (strstr(help, "-vnc"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_VNC);
    if (strstr(help, "seamless-migration="))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_SEAMLESS_MIGRATION);
    if (strstr(help, "boot=on"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_BOOT);
    if (strstr(help, "serial=s"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_SERIAL);
    if (strstr(help, "-pcidevice"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_PCIDEVICE);
    if (strstr(help, "-mem-path"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_MEM_PATH);
    if (strstr(help, "-chardev")) {
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_CHARDEV);
        if (strstr(help, "-chardev spicevmc"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_CHARDEV_SPICEVMC);
    }
    if (strstr(help, "-balloon"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_BALLOON);
    if (strstr(help, "-device")) {
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_DEVICE);
        /*
         * When -device was introduced, qemu already supported drive's
         * readonly option but didn't advertise that.
         */
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_READONLY);
    }
    if (strstr(help, "-nodefconfig"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NODEFCONFIG);
    if (strstr(help, "-no-user-config"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NO_USER_CONFIG);
    /* The trailing ' ' is important to avoid a bogus match */
    if (strstr(help, "-rtc "))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_RTC);
    /* to wit */
    if (strstr(help, "-rtc-td-hack"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_RTC_TD_HACK);
    if (strstr(help, "-no-hpet"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NO_HPET);
    if (strstr(help, "-no-acpi"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NO_ACPI);
    if (strstr(help, "-no-kvm-pit-reinjection"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NO_KVM_PIT);
    if (strstr(help, "-tdf"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_TDF);
    if (strstr(help, "-enable-nesting"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NESTING);
    if (strstr(help, ",menu=on"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_BOOT_MENU);
    if (strstr(help, ",reboot-timeout=rb_time"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_REBOOT_TIMEOUT);
    if ((fsdev = strstr(help, "-fsdev"))) {
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_FSDEV);
        if (strstr(fsdev, "readonly"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_FSDEV_READONLY);
        if (strstr(fsdev, "writeout"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_FSDEV_WRITEOUT);
    }
    if (strstr(help, "-smbios type"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_SMBIOS_TYPE);
    if (strstr(help, "-sandbox"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_SECCOMP_SANDBOX);

    if ((netdev = strstr(help, "-netdev"))) {
        /* Disable -netdev on 0.12 since although it exists,
         * the corresponding netdev_add/remove monitor commands
         * do not, and we need them to be able to do hotplug.
         * But see below about RHEL build. */
        if (version >= 13000) {
            if (strstr(netdev, "bridge"))
                virQEMUCapsSet(qemuCaps, QEMU_CAPS_NETDEV_BRIDGE);
           virQEMUCapsSet(qemuCaps, QEMU_CAPS_NETDEV);
        }
    }

    if (strstr(help, "-sdl"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_SDL);
    if (strstr(help, "cores=") &&
        strstr(help, "threads=") &&
        strstr(help, "sockets="))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_SMP_TOPOLOGY);

    if (version >= 9000)
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_VNC_COLON);

    if (is_kvm && (version >= 10000 || kvm_version >= 74))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_VNET_HDR);

    if (strstr(help, ",vhost=")) {
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_VHOST_NET);
    }

    /* Do not use -no-shutdown if qemu doesn't support it or SIGTERM handling
     * is most likely buggy when used with -no-shutdown (which applies for qemu
     * 0.14.* and 0.15.0)
     */
    if (strstr(help, "-no-shutdown") && (version < 14000 || version > 15000))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NO_SHUTDOWN);

    if (strstr(help, "dump-guest-core=on|off"))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_DUMP_GUEST_CORE);

    /*
     * Handling of -incoming arg with varying features
     *  -incoming tcp    (kvm >= 79, qemu >= 0.10.0)
     *  -incoming exec   (kvm >= 80, qemu >= 0.10.0)
     *  -incoming unix   (qemu >= 0.12.0)
     *  -incoming fd     (qemu >= 0.12.0)
     *  -incoming stdio  (all earlier kvm)
     *
     * NB, there was a pre-kvm-79 'tcp' support, but it
     * was broken, because it blocked the monitor console
     * while waiting for data, so pretend it doesn't exist
     */
    if (version >= 10000) {
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_TCP);
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_EXEC);
        if (version >= 12000) {
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_UNIX);
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_FD);
        }
    } else if (kvm_version >= 79) {
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_TCP);
        if (kvm_version >= 80)
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_EXEC);
    } else if (kvm_version > 0) {
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_MIGRATE_KVM_STDIO);
    }

    if (version >= 10000)
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_0_10);

    if (version >= 11000)
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_VIRTIO_BLK_SG_IO);

    /* While JSON mode was available in 0.12.0, it was too
     * incomplete to contemplate using. The 0.13.0 release
     * is good enough to use, even though it lacks one or
     * two features. This is also true of versions of qemu
     * built for RHEL, labeled 0.12.1, but with extra text
     * in the help output that mentions that features were
     * backported for libvirt. The benefits of JSON mode now
     * outweigh the downside.
     */
#if WITH_YAJL
    if (version >= 13000) {
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_MONITOR_JSON);
    } else if (version >= 12000 &&
               strstr(help, "libvirt")) {
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_MONITOR_JSON);
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NETDEV);
    }
#else
    /* Starting with qemu 0.15 and newer, upstream qemu no longer
     * promises to keep the human interface stable, but requests that
     * we use QMP (the JSON interface) for everything.  If the user
     * forgot to include YAJL libraries when building their own
     * libvirt but is targetting a newer qemu, we are better off
     * telling them to recompile (the spec file includes the
     * dependency, so distros won't hit this).  This check is
     * also in m4/virt-yajl.m4 (see $with_yajl).  */
    if (version >= 15000 ||
        (version >= 12000 && strstr(help, "libvirt"))) {
        if (check_yajl) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("this qemu binary requires libvirt to be "
                             "compiled with yajl"));
            return -1;
        }
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NETDEV);
    }
#endif

    if (version >= 13000)
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_PCI_MULTIFUNCTION);

    /* Although very new versions of qemu advertise the presence of
     * the rombar option in the output of "qemu -device pci-assign,?",
     * this advertisement was added to the code long after the option
     * itself. According to qemu developers, though, rombar is
     * available in all qemu binaries from release 0.12 onward.
     * Setting the capability this way makes it available in more
     * cases where it might be needed, and shouldn't cause any false
     * positives (in the case that it did, qemu would produce an error
     * log and refuse to start, so it would be immediately obvious).
     */
    if (version >= 12000)
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_PCI_ROMBAR);

    if (version >= 11000)
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_CPU_HOST);

    if (version >= 1002000)
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_DEVICE_VIDEO_PRIMARY);
    return 0;
}

/* We parse the output of 'qemu -help' to get the QEMU
 * version number. The first bit is easy, just parse
 * 'QEMU PC emulator version x.y.z'
 * or
 * 'QEMU emulator version x.y.z'.
 *
 * With qemu-kvm, however, that is followed by a string
 * in parenthesis as follows:
 *  - qemu-kvm-x.y.z in stable releases
 *  - kvm-XX for kvm versions up to kvm-85
 *  - qemu-kvm-devel-XX for kvm version kvm-86 and later
 *
 * For qemu-kvm versions before 0.10.z, we need to detect
 * the KVM version number for some features. With 0.10.z
 * and later, we just need the QEMU version number and
 * whether it is KVM QEMU or mainline QEMU.
 */
#define QEMU_VERSION_STR_1  "QEMU emulator version"
#define QEMU_VERSION_STR_2  "QEMU PC emulator version"
#define QEMU_KVM_VER_PREFIX "(qemu-kvm-"
#define KVM_VER_PREFIX      "(kvm-"

#define SKIP_BLANKS(p) do { while ((*(p) == ' ') || (*(p) == '\t')) (p)++; } while (0)

int virQEMUCapsParseHelpStr(const char *qemu,
                            const char *help,
                            virQEMUCapsPtr qemuCaps,
                            unsigned int *version,
                            unsigned int *is_kvm,
                            unsigned int *kvm_version,
                            bool check_yajl)
{
    unsigned major, minor, micro;
    const char *p = help;
    char *strflags;

    *version = *is_kvm = *kvm_version = 0;

    if (STRPREFIX(p, QEMU_VERSION_STR_1))
        p += strlen(QEMU_VERSION_STR_1);
    else if (STRPREFIX(p, QEMU_VERSION_STR_2))
        p += strlen(QEMU_VERSION_STR_2);
    else
        goto fail;

    SKIP_BLANKS(p);

    major = virParseNumber(&p);
    if (major == -1 || *p != '.')
        goto fail;

    ++p;

    minor = virParseNumber(&p);
    if (minor == -1)
        goto fail;

    if (*p != '.') {
        micro = 0;
    } else {
        ++p;
        micro = virParseNumber(&p);
        if (micro == -1)
            goto fail;
    }

    SKIP_BLANKS(p);

    if (STRPREFIX(p, QEMU_KVM_VER_PREFIX)) {
        *is_kvm = 1;
        p += strlen(QEMU_KVM_VER_PREFIX);
    } else if (STRPREFIX(p, KVM_VER_PREFIX)) {
        int ret;

        *is_kvm = 1;
        p += strlen(KVM_VER_PREFIX);

        ret = virParseNumber(&p);
        if (ret == -1)
            goto fail;

        *kvm_version = ret;
    }

    *version = (major * 1000 * 1000) + (minor * 1000) + micro;

    if (virQEMUCapsComputeCmdFlags(help, *version, *is_kvm, *kvm_version,
                                   qemuCaps, check_yajl) < 0)
        goto cleanup;

    strflags = virBitmapString(qemuCaps->flags);
    VIR_DEBUG("Version %u.%u.%u, cooked version %u, flags %s",
              major, minor, micro, *version, NULLSTR(strflags));
    VIR_FREE(strflags);

    if (*kvm_version)
        VIR_DEBUG("KVM version %d detected", *kvm_version);
    else if (*is_kvm)
        VIR_DEBUG("qemu-kvm version %u.%u.%u detected", major, minor, micro);

    return 0;

fail:
    p = strchr(help, '\n');
    if (!p)
        p = strchr(help, '\0');

    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("cannot parse %s version number in '%.*s'"),
                   qemu, (int) (p - help), help);

cleanup:
    return -1;
}


struct virQEMUCapsStringFlags {
    const char *value;
    int flag;
};


struct virQEMUCapsStringFlags virQEMUCapsObjectTypes[] = {
    { "hda-duplex", QEMU_CAPS_HDA_DUPLEX },
    { "hda-micro", QEMU_CAPS_HDA_MICRO },
    { "ccid-card-emulated", QEMU_CAPS_CCID_EMULATED },
    { "ccid-card-passthru", QEMU_CAPS_CCID_PASSTHRU },
    { "piix3-usb-uhci", QEMU_CAPS_PIIX3_USB_UHCI },
    { "piix4-usb-uhci", QEMU_CAPS_PIIX4_USB_UHCI },
    { "usb-ehci", QEMU_CAPS_USB_EHCI },
    { "ich9-usb-ehci1", QEMU_CAPS_ICH9_USB_EHCI1 },
    { "vt82c686b-usb-uhci", QEMU_CAPS_VT82C686B_USB_UHCI },
    { "pci-ohci", QEMU_CAPS_PCI_OHCI },
    { "nec-usb-xhci", QEMU_CAPS_NEC_USB_XHCI },
    { "usb-redir", QEMU_CAPS_USB_REDIR },
    { "usb-hub", QEMU_CAPS_USB_HUB },
    { "ich9-ahci", QEMU_CAPS_ICH9_AHCI },
    { "virtio-blk-s390", QEMU_CAPS_VIRTIO_S390 },
    { "sclpconsole", QEMU_CAPS_SCLP_S390 },
    { "lsi53c895a", QEMU_CAPS_SCSI_LSI },
    { "virtio-scsi-pci", QEMU_CAPS_VIRTIO_SCSI_PCI },
    { "spicevmc", QEMU_CAPS_DEVICE_SPICEVMC },
    { "qxl-vga", QEMU_CAPS_DEVICE_QXL_VGA },
    { "qxl", QEMU_CAPS_DEVICE_QXL },
    { "sga", QEMU_CAPS_SGA },
    { "scsi-block", QEMU_CAPS_SCSI_BLOCK },
    { "scsi-cd", QEMU_CAPS_SCSI_CD },
    { "ide-cd", QEMU_CAPS_IDE_CD },
    { "VGA", QEMU_CAPS_DEVICE_VGA },
    { "cirrus-vga", QEMU_CAPS_DEVICE_CIRRUS_VGA },
    { "vmware-svga", QEMU_CAPS_DEVICE_VMWARE_SVGA },
    { "usb-serial", QEMU_CAPS_DEVICE_USB_SERIAL},
    { "usb-net", QEMU_CAPS_DEVICE_USB_NET},
    { "virtio-rng-pci", QEMU_CAPS_DEVICE_VIRTIO_RNG },
    { "rng-random", QEMU_CAPS_OBJECT_RNG_RANDOM },
    { "rng-egd", QEMU_CAPS_OBJECT_RNG_EGD },
};


static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsVirtioBlk[] = {
    { "multifunction", QEMU_CAPS_PCI_MULTIFUNCTION },
    { "bootindex", QEMU_CAPS_BOOTINDEX },
    { "ioeventfd", QEMU_CAPS_VIRTIO_IOEVENTFD },
    { "event_idx", QEMU_CAPS_VIRTIO_BLK_EVENT_IDX },
    { "scsi", QEMU_CAPS_VIRTIO_BLK_SCSI },
    { "logical_block_size", QEMU_CAPS_BLOCKIO },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsVirtioNet[] = {
    { "tx", QEMU_CAPS_VIRTIO_TX_ALG },
    { "event_idx", QEMU_CAPS_VIRTIO_NET_EVENT_IDX },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsPciAssign[] = {
    { "rombar", QEMU_CAPS_PCI_ROMBAR },
    { "configfd", QEMU_CAPS_PCI_CONFIGFD },
    { "bootindex", QEMU_CAPS_PCI_BOOTINDEX },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsScsiDisk[] = {
    { "channel", QEMU_CAPS_SCSI_DISK_CHANNEL },
    { "wwn", QEMU_CAPS_SCSI_DISK_WWN },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsIDEDrive[] = {
    { "wwn", QEMU_CAPS_IDE_DRIVE_WWN },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsPixx4PM[] = {
    { "disable_s3", QEMU_CAPS_DISABLE_S3 },
    { "disable_s4", QEMU_CAPS_DISABLE_S4 },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsUsbRedir[] = {
    { "filter", QEMU_CAPS_USB_REDIR_FILTER },
    { "bootindex", QEMU_CAPS_USB_REDIR_BOOTINDEX },
};

static struct virQEMUCapsStringFlags virQEMUCapsObjectPropsUsbHost[] = {
    { "bootindex", QEMU_CAPS_USB_HOST_BOOTINDEX },
};

struct virQEMUCapsObjectTypeProps {
    const char *type;
    struct virQEMUCapsStringFlags *props;
    size_t nprops;
};

static struct virQEMUCapsObjectTypeProps virQEMUCapsObjectProps[] = {
    { "virtio-blk-pci", virQEMUCapsObjectPropsVirtioBlk,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsVirtioBlk) },
    { "virtio-net-pci", virQEMUCapsObjectPropsVirtioNet,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsVirtioNet) },
    { "virtio-blk-s390", virQEMUCapsObjectPropsVirtioBlk,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsVirtioBlk) },
    { "virtio-net-s390", virQEMUCapsObjectPropsVirtioNet,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsVirtioNet) },
    { "pci-assign", virQEMUCapsObjectPropsPciAssign,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsPciAssign) },
    { "kvm-pci-assign", virQEMUCapsObjectPropsPciAssign,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsPciAssign) },
    { "scsi-disk", virQEMUCapsObjectPropsScsiDisk,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsScsiDisk) },
    { "ide-drive", virQEMUCapsObjectPropsIDEDrive,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsIDEDrive) },
    { "PIIX4_PM", virQEMUCapsObjectPropsPixx4PM,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsPixx4PM) },
    { "usb-redir", virQEMUCapsObjectPropsUsbRedir,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsUsbRedir) },
    { "usb-host", virQEMUCapsObjectPropsUsbHost,
      ARRAY_CARDINALITY(virQEMUCapsObjectPropsUsbHost) },
};


static void
virQEMUCapsProcessStringFlags(virQEMUCapsPtr qemuCaps,
                              size_t nflags,
                              struct virQEMUCapsStringFlags *flags,
                              size_t nvalues,
                              char *const*values)
{
    size_t i, j;
    for (i = 0 ; i < nflags ; i++) {
        for (j = 0 ; j < nvalues ; j++) {
            if (STREQ(values[j], flags[i].value)) {
                virQEMUCapsSet(qemuCaps, flags[i].flag);
                break;
            }
        }
    }
}


static void
virQEMUCapsFreeStringList(size_t len,
                          char **values)
{
    size_t i;
    for (i = 0 ; i < len ; i++)
        VIR_FREE(values[i]);
    VIR_FREE(values);
}


#define OBJECT_TYPE_PREFIX "name \""

static int
virQEMUCapsParseDeviceStrObjectTypes(const char *str,
                                     char ***types)
{
    const char *tmp = str;
    int ret = -1;
    size_t ntypelist = 0;
    char **typelist = NULL;

    *types = NULL;

    while ((tmp = strstr(tmp, OBJECT_TYPE_PREFIX))) {
        char *end;
        tmp += strlen(OBJECT_TYPE_PREFIX);
        end = strstr(tmp, "\"");
        if (!end) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Malformed QEMU device list string, missing quote"));
            goto cleanup;
        }

        if (VIR_EXPAND_N(typelist, ntypelist, 1) < 0) {
            virReportOOMError();
            goto cleanup;
        }
        if (!(typelist[ntypelist-1] = strndup(tmp, end-tmp))) {
            virReportOOMError();
            goto cleanup;
        }
    }

    *types = typelist;
    ret = ntypelist;

cleanup:
    if (ret < 0)
        virQEMUCapsFreeStringList(ntypelist, typelist);
    return ret;
}


static int
virQEMUCapsParseDeviceStrObjectProps(const char *str,
                                     const char *type,
                                     char ***props)
{
    const char *tmp = str;
    int ret = -1;
    size_t nproplist = 0;
    char **proplist = NULL;

    VIR_DEBUG("Extract type %s", type);
    *props = NULL;

    while ((tmp = strchr(tmp, '\n'))) {
        char *end;
        tmp += 1;

        if (*tmp == '\0')
            break;

        if (STRPREFIX(tmp, OBJECT_TYPE_PREFIX))
            continue;

        if (!STRPREFIX(tmp, type))
            continue;

        tmp += strlen(type);
        if (*tmp != '.')
            continue;
        tmp++;

        end = strstr(tmp, "=");
        if (!end) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Malformed QEMU device list string, missing '='"));
            goto cleanup;
        }
        if (VIR_EXPAND_N(proplist, nproplist, 1) < 0) {
            virReportOOMError();
            goto cleanup;
        }
        if (!(proplist[nproplist-1] = strndup(tmp, end-tmp))) {
            virReportOOMError();
            goto cleanup;
        }
    }

    *props = proplist;
    ret = nproplist;

cleanup:
    if (ret < 0)
        virQEMUCapsFreeStringList(nproplist, proplist);
    return ret;
}


int
virQEMUCapsParseDeviceStr(virQEMUCapsPtr qemuCaps, const char *str)
{
    int nvalues;
    char **values;
    size_t i;

    if ((nvalues = virQEMUCapsParseDeviceStrObjectTypes(str, &values)) < 0)
        return -1;
    virQEMUCapsProcessStringFlags(qemuCaps,
                                  ARRAY_CARDINALITY(virQEMUCapsObjectTypes),
                                  virQEMUCapsObjectTypes,
                                  nvalues, values);
    virQEMUCapsFreeStringList(nvalues, values);

    for (i = 0 ; i < ARRAY_CARDINALITY(virQEMUCapsObjectProps); i++) {
        const char *type = virQEMUCapsObjectProps[i].type;
        if ((nvalues = virQEMUCapsParseDeviceStrObjectProps(str,
                                                            type,
                                                            &values)) < 0)
            return -1;
        virQEMUCapsProcessStringFlags(qemuCaps,
                                      virQEMUCapsObjectProps[i].nprops,
                                      virQEMUCapsObjectProps[i].props,
                                      nvalues, values);
        virQEMUCapsFreeStringList(nvalues, values);
    }

    /* Prefer -chardev spicevmc (detected earlier) over -device spicevmc */
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_CHARDEV_SPICEVMC))
        virQEMUCapsClear(qemuCaps, QEMU_CAPS_DEVICE_SPICEVMC);

    return 0;
}


static int
virQEMUCapsExtractDeviceStr(const char *qemu,
                            virQEMUCapsPtr qemuCaps,
                            uid_t runUid, gid_t runGid)
{
    char *output = NULL;
    virCommandPtr cmd;
    int ret = -1;

    /* Cram together all device-related queries into one invocation;
     * the output format makes it possible to distinguish what we
     * need.  With qemu 0.13.0 and later, unrecognized '-device
     * bogus,?' cause an error in isolation, but are silently ignored
     * in combination with '-device ?'.  Upstream qemu 0.12.x doesn't
     * understand '-device name,?', and always exits with status 1 for
     * the simpler '-device ?', so this function is really only useful
     * if -help includes "device driver,?".  */
    cmd = virQEMUCapsProbeCommand(qemu, qemuCaps, runUid, runGid);
    virCommandAddArgList(cmd,
                         "-device", "?",
                         "-device", "pci-assign,?",
                         "-device", "virtio-blk-pci,?",
                         "-device", "virtio-net-pci,?",
                         "-device", "scsi-disk,?",
                         "-device", "PIIX4_PM,?",
                         "-device", "usb-redir,?",
                         "-device", "ide-drive,?",
                         "-device", "usb-host,?",
                         NULL);
    /* qemu -help goes to stdout, but qemu -device ? goes to stderr.  */
    virCommandSetErrorBuffer(cmd, &output);

    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;

    ret = virQEMUCapsParseDeviceStr(qemuCaps, output);

cleanup:
    VIR_FREE(output);
    virCommandFree(cmd);
    return ret;
}


int virQEMUCapsGetDefaultVersion(virCapsPtr caps,
                                 virQEMUCapsCachePtr capsCache,
                                 unsigned int *version)
{
    const char *binary;
    virQEMUCapsPtr qemucaps;

    if (*version > 0)
        return 0;

    if ((binary = virCapabilitiesDefaultGuestEmulator(caps,
                                                      "hvm",
                                                      virArchFromHost(),
                                                      "qemu")) == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Cannot find suitable emulator for %s"),
                       virArchToString(virArchFromHost()));
        return -1;
    }

    if (!(qemucaps = virQEMUCapsCacheLookup(capsCache, binary)))
        return -1;

    *version = virQEMUCapsGetVersion(qemucaps);
    virObjectUnref(qemucaps);
    return 0;
}




virQEMUCapsPtr
virQEMUCapsNew(void)
{
    virQEMUCapsPtr qemuCaps;

    if (virQEMUCapsInitialize() < 0)
        return NULL;

    if (!(qemuCaps = virObjectNew(virQEMUCapsClass)))
        return NULL;

    if (!(qemuCaps->flags = virBitmapNew(QEMU_CAPS_LAST)))
        goto no_memory;

    return qemuCaps;

no_memory:
    virReportOOMError();
    virObjectUnref(qemuCaps);
    return NULL;
}


virQEMUCapsPtr virQEMUCapsNewCopy(virQEMUCapsPtr qemuCaps)
{
    virQEMUCapsPtr ret = virQEMUCapsNew();
    size_t i;

    if (!ret)
        return NULL;

    virBitmapCopy(ret->flags, qemuCaps->flags);

    ret->usedQMP = qemuCaps->usedQMP;
    ret->version = qemuCaps->version;
    ret->kvmVersion = qemuCaps->kvmVersion;
    ret->arch = qemuCaps->arch;

    if (VIR_ALLOC_N(ret->cpuDefinitions, qemuCaps->ncpuDefinitions) < 0)
        goto no_memory;
    ret->ncpuDefinitions = qemuCaps->ncpuDefinitions;
    for (i = 0 ; i < qemuCaps->ncpuDefinitions ; i++) {
        if (!(ret->cpuDefinitions[i] = strdup(qemuCaps->cpuDefinitions[i])))
            goto no_memory;
    }

    if (VIR_ALLOC_N(ret->machineTypes, qemuCaps->nmachineTypes) < 0)
        goto no_memory;
    if (VIR_ALLOC_N(ret->machineAliases, qemuCaps->nmachineTypes) < 0)
        goto no_memory;
    ret->nmachineTypes = qemuCaps->nmachineTypes;
    for (i = 0 ; i < qemuCaps->nmachineTypes ; i++) {
        if (!(ret->machineTypes[i] = strdup(qemuCaps->machineTypes[i])))
            goto no_memory;
        if (qemuCaps->machineAliases[i] &&
            !(ret->machineAliases[i] = strdup(qemuCaps->machineAliases[i])))
            goto no_memory;
    }

    return ret;

no_memory:
    virReportOOMError();
    virObjectUnref(ret);
    return NULL;
}


void virQEMUCapsDispose(void *obj)
{
    virQEMUCapsPtr qemuCaps = obj;
    size_t i;

    for (i = 0 ; i < qemuCaps->nmachineTypes ; i++) {
        VIR_FREE(qemuCaps->machineTypes[i]);
        VIR_FREE(qemuCaps->machineAliases[i]);
    }
    VIR_FREE(qemuCaps->machineTypes);
    VIR_FREE(qemuCaps->machineAliases);

    for (i = 0 ; i < qemuCaps->ncpuDefinitions ; i++) {
        VIR_FREE(qemuCaps->cpuDefinitions[i]);
    }
    VIR_FREE(qemuCaps->cpuDefinitions);

    virBitmapFree(qemuCaps->flags);

    VIR_FREE(qemuCaps->binary);
}

void
virQEMUCapsSet(virQEMUCapsPtr qemuCaps,
               enum virQEMUCapsFlags flag)
{
    ignore_value(virBitmapSetBit(qemuCaps->flags, flag));
}


void
virQEMUCapsSetList(virQEMUCapsPtr qemuCaps, ...)
{
    va_list list;
    int flag;

    va_start(list, qemuCaps);
    while ((flag = va_arg(list, int)) < QEMU_CAPS_LAST)
        ignore_value(virBitmapSetBit(qemuCaps->flags, flag));
    va_end(list);
}


void
virQEMUCapsClear(virQEMUCapsPtr qemuCaps,
                 enum virQEMUCapsFlags flag)
{
    ignore_value(virBitmapClearBit(qemuCaps->flags, flag));
}


char *virQEMUCapsFlagsString(virQEMUCapsPtr qemuCaps)
{
    return virBitmapString(qemuCaps->flags);
}


bool
virQEMUCapsGet(virQEMUCapsPtr qemuCaps,
               enum virQEMUCapsFlags flag)
{
    bool b;

    if (!qemuCaps || virBitmapGetBit(qemuCaps->flags, flag, &b) < 0)
        return false;
    else
        return b;
}


const char *virQEMUCapsGetBinary(virQEMUCapsPtr qemuCaps)
{
    return qemuCaps->binary;
}

virArch virQEMUCapsGetArch(virQEMUCapsPtr qemuCaps)
{
    return qemuCaps->arch;
}


unsigned int virQEMUCapsGetVersion(virQEMUCapsPtr qemuCaps)
{
    return qemuCaps->version;
}


unsigned int virQEMUCapsGetKVMVersion(virQEMUCapsPtr qemuCaps)
{
    return qemuCaps->kvmVersion;
}


int virQEMUCapsAddCPUDefinition(virQEMUCapsPtr qemuCaps,
                                const char *name)
{
    char *tmp = strdup(name);
    if (!tmp) {
        virReportOOMError();
        return -1;
    }
    if (VIR_EXPAND_N(qemuCaps->cpuDefinitions, qemuCaps->ncpuDefinitions, 1) < 0) {
        VIR_FREE(tmp);
        virReportOOMError();
        return -1;
    }
    qemuCaps->cpuDefinitions[qemuCaps->ncpuDefinitions-1] = tmp;
    return 0;
}


size_t virQEMUCapsGetCPUDefinitions(virQEMUCapsPtr qemuCaps,
                                    char ***names)
{
    if (names)
        *names = qemuCaps->cpuDefinitions;
    return qemuCaps->ncpuDefinitions;
}


size_t virQEMUCapsGetMachineTypes(virQEMUCapsPtr qemuCaps,
                                  char ***names)
{
    if (names)
        *names = qemuCaps->machineTypes;
    return qemuCaps->nmachineTypes;
}

int virQEMUCapsGetMachineTypesCaps(virQEMUCapsPtr qemuCaps,
                                   size_t *nmachines,
                                   virCapsGuestMachinePtr **machines)
{
    size_t i;

    *nmachines = 0;
    *machines = NULL;
    if (VIR_ALLOC_N(*machines, qemuCaps->nmachineTypes) < 0)
        goto no_memory;
    *nmachines = qemuCaps->nmachineTypes;

    for (i = 0 ; i < qemuCaps->nmachineTypes ; i++) {
        virCapsGuestMachinePtr mach;
        if (VIR_ALLOC(mach) < 0)
            goto no_memory;
        if (qemuCaps->machineAliases[i]) {
            if (!(mach->name = strdup(qemuCaps->machineAliases[i])))
                goto no_memory;
            if (!(mach->canonical = strdup(qemuCaps->machineTypes[i])))
                goto no_memory;
        } else {
            if (!(mach->name = strdup(qemuCaps->machineTypes[i])))
                goto no_memory;
        }
        (*machines)[i] = mach;
    }

    return 0;

no_memory:
    virCapabilitiesFreeMachines(*machines, *nmachines);
    *nmachines = 0;
    *machines = NULL;
    return -1;
}




const char *virQEMUCapsGetCanonicalMachine(virQEMUCapsPtr qemuCaps,
                                           const char *name)
{
    size_t i;

    if (!name)
        return NULL;

    for (i = 0 ; i < qemuCaps->nmachineTypes ; i++) {
        if (!qemuCaps->machineAliases[i])
            continue;
        if (STREQ(qemuCaps->machineAliases[i], name))
            return qemuCaps->machineTypes[i];
    }

    return name;
}


static int
virQEMUCapsProbeQMPCommands(virQEMUCapsPtr qemuCaps,
                            qemuMonitorPtr mon)
{
    char **commands = NULL;
    int ncommands;
    size_t i;

    if ((ncommands = qemuMonitorGetCommands(mon, &commands)) < 0)
        return -1;

    for (i = 0 ; i < ncommands ; i++) {
        char *name = commands[i];
        if (STREQ(name, "system_wakeup"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_WAKEUP);
        else if (STREQ(name, "transaction"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_TRANSACTION);
        else if (STREQ(name, "block_job_cancel"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_BLOCKJOB_SYNC);
        else if (STREQ(name, "block-job-cancel"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_BLOCKJOB_ASYNC);
        else if (STREQ(name, "dump-guest-memory"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_DUMP_GUEST_MEMORY);
        else if (STREQ(name, "query-spice"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_SPICE);
        else if (STREQ(name, "query-kvm"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_KVM);
        else if (STREQ(name, "block-commit"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_BLOCK_COMMIT);
        else if (STREQ(name, "query-vnc"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_VNC);
        else if (STREQ(name, "drive-mirror"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_MIRROR);
        else if (STREQ(name, "blockdev-snapshot-sync"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_DISK_SNAPSHOT);
        else if (STREQ(name, "add-fd"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_ADD_FD);
        else if (STREQ(name, "nbd-server-start"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_NBD_SERVER);
        VIR_FREE(name);
    }
    VIR_FREE(commands);

    /* QMP add-fd was introduced in 1.2, but did not support
     * management control of set numbering, and did not have a
     * counterpart -add-fd command line option.  We require the
     * add-fd features from 1.3 or later.  */
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_ADD_FD)) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("unable to probe for add-fd"));
            return -1;
        }
        if (qemuMonitorAddFd(mon, 0, fd, "/dev/null") < 0)
            virQEMUCapsClear(qemuCaps, QEMU_CAPS_ADD_FD);
        VIR_FORCE_CLOSE(fd);
    }

    return 0;
}


static int
virQEMUCapsProbeQMPEvents(virQEMUCapsPtr qemuCaps,
                          qemuMonitorPtr mon)
{
    char **events = NULL;
    int nevents;
    size_t i;

    if ((nevents = qemuMonitorGetEvents(mon, &events)) < 0)
        return -1;

    for (i = 0 ; i < nevents ; i++) {
        char *name = events[i];

        if (STREQ(name, "BALLOON_CHANGE"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_BALLOON_EVENT);
        if (STREQ(name, "SPICE_MIGRATE_COMPLETED"))
            virQEMUCapsSet(qemuCaps, QEMU_CAPS_SEAMLESS_MIGRATION);
        VIR_FREE(name);
    }
    VIR_FREE(events);

    return 0;
}


static int
virQEMUCapsProbeQMPObjects(virQEMUCapsPtr qemuCaps,
                           qemuMonitorPtr mon)
{
    int nvalues;
    char **values;
    size_t i;

    if ((nvalues = qemuMonitorGetObjectTypes(mon, &values)) < 0)
        return -1;
    virQEMUCapsProcessStringFlags(qemuCaps,
                                  ARRAY_CARDINALITY(virQEMUCapsObjectTypes),
                                  virQEMUCapsObjectTypes,
                                  nvalues, values);
    virQEMUCapsFreeStringList(nvalues, values);

    for (i = 0 ; i < ARRAY_CARDINALITY(virQEMUCapsObjectProps); i++) {
        const char *type = virQEMUCapsObjectProps[i].type;
        if ((nvalues = qemuMonitorGetObjectProps(mon,
                                                 type,
                                                 &values)) < 0)
            return -1;
        virQEMUCapsProcessStringFlags(qemuCaps,
                                      virQEMUCapsObjectProps[i].nprops,
                                      virQEMUCapsObjectProps[i].props,
                                      nvalues, values);
        virQEMUCapsFreeStringList(nvalues, values);
    }

    /* Prefer -chardev spicevmc (detected earlier) over -device spicevmc */
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_CHARDEV_SPICEVMC))
        virQEMUCapsClear(qemuCaps, QEMU_CAPS_DEVICE_SPICEVMC);
    /* If qemu supports newer -device qxl it supports -vga qxl as well */
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE_QXL))
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_VGA_QXL);

    return 0;
}


static int
virQEMUCapsProbeQMPMachineTypes(virQEMUCapsPtr qemuCaps,
                                qemuMonitorPtr mon)
{
    qemuMonitorMachineInfoPtr *machines = NULL;
    int nmachines = 0;
    int ret = -1;
    size_t i;
    size_t defIdx = 0;

    if ((nmachines = qemuMonitorGetMachines(mon, &machines)) < 0)
        goto cleanup;

    if (VIR_ALLOC_N(qemuCaps->machineTypes, nmachines) < 0) {
        virReportOOMError();
        goto cleanup;
    }
    if (VIR_ALLOC_N(qemuCaps->machineAliases, nmachines) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    for (i = 0 ; i < nmachines ; i++) {
        if (machines[i]->alias) {
            if (!(qemuCaps->machineAliases[i] = strdup(machines[i]->alias))) {
                virReportOOMError();
                goto cleanup;
            }
        }
        if (!(qemuCaps->machineTypes[i] = strdup(machines[i]->name))) {
            virReportOOMError();
            goto cleanup;
        }
        if (machines[i]->isDefault)
            defIdx = i;
    }
    qemuCaps->nmachineTypes = nmachines;

    if (defIdx)
        virQEMUCapsSetDefaultMachine(qemuCaps, defIdx);

    ret = 0;

cleanup:
    for (i = 0 ; i < nmachines ; i++)
        qemuMonitorMachineInfoFree(machines[i]);
    VIR_FREE(machines);
    return ret;
}


static int
virQEMUCapsProbeQMPCPUDefinitions(virQEMUCapsPtr qemuCaps,
                                  qemuMonitorPtr mon)
{
    int ncpuDefinitions;
    char **cpuDefinitions;

    if ((ncpuDefinitions = qemuMonitorGetCPUDefinitions(mon, &cpuDefinitions)) < 0)
        return -1;

    qemuCaps->ncpuDefinitions = ncpuDefinitions;
    qemuCaps->cpuDefinitions = cpuDefinitions;

    return 0;
}


static int
virQEMUCapsProbeQMPKVMState(virQEMUCapsPtr qemuCaps,
                            qemuMonitorPtr mon)
{
    bool enabled = false;
    bool present = false;

    if (!virQEMUCapsGet(qemuCaps, QEMU_CAPS_KVM))
        return 0;

    if (qemuMonitorGetKVMState(mon, &enabled, &present) < 0)
        return -1;

    /* The QEMU_CAPS_KVM flag was initially set according to the QEMU
     * reporting the recognition of 'query-kvm' QMP command. That merely
     * indicates existance of the command though, not whether KVM support
     * is actually available, nor whether it is enabled by default.
     *
     * If it is not present we need to clear the flag, and if it is
     * not enabled by default we need to change the flag.
     */
    if (!present) {
        virQEMUCapsClear(qemuCaps, QEMU_CAPS_KVM);
    } else if (!enabled) {
        virQEMUCapsClear(qemuCaps, QEMU_CAPS_KVM);
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_ENABLE_KVM);
    }

    return 0;
}


int virQEMUCapsProbeQMP(virQEMUCapsPtr qemuCaps,
                        qemuMonitorPtr mon)
{
    VIR_DEBUG("qemuCaps=%p mon=%p", qemuCaps, mon);

    if (qemuCaps->usedQMP)
        return 0;

    if (virQEMUCapsProbeQMPCommands(qemuCaps, mon) < 0)
        return -1;

    if (virQEMUCapsProbeQMPEvents(qemuCaps, mon) < 0)
        return -1;

    return 0;
}


#define QEMU_SYSTEM_PREFIX "qemu-system-"

static int
virQEMUCapsInitHelp(virQEMUCapsPtr qemuCaps, uid_t runUid, gid_t runGid)
{
    virCommandPtr cmd = NULL;
    unsigned int is_kvm;
    char *help = NULL;
    int ret = -1;
    const char *tmp;

    VIR_DEBUG("qemuCaps=%p", qemuCaps);

    tmp = strstr(qemuCaps->binary, QEMU_SYSTEM_PREFIX);
    if (tmp) {
        tmp += strlen(QEMU_SYSTEM_PREFIX);

        qemuCaps->arch = virQEMUCapsArchFromString(tmp);
    } else {
        qemuCaps->arch = virArchFromHost();
    }

    cmd = virQEMUCapsProbeCommand(qemuCaps->binary, NULL, runUid, runGid);
    virCommandAddArgList(cmd, "-help", NULL);
    virCommandSetOutputBuffer(cmd, &help);

    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;

    if (virQEMUCapsParseHelpStr(qemuCaps->binary,
                                help, qemuCaps,
                                &qemuCaps->version,
                                &is_kvm,
                                &qemuCaps->kvmVersion,
                                false) < 0)
        goto cleanup;

    /* Currently only x86_64 and i686 support PCI-multibus. */
    if (qemuCaps->arch == VIR_ARCH_X86_64 ||
        qemuCaps->arch == VIR_ARCH_I686) {
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_PCI_MULTIBUS);
    } else {
        /* -no-acpi is not supported on other archs
         * even if qemu reports it in -help */
        virQEMUCapsClear(qemuCaps, QEMU_CAPS_NO_ACPI);
    }

    /* virQEMUCapsExtractDeviceStr will only set additional caps if qemu
     * understands the 0.13.0+ notion of "-device driver,".  */
    if (virQEMUCapsGet(qemuCaps, QEMU_CAPS_DEVICE) &&
        strstr(help, "-device driver,?") &&
        virQEMUCapsExtractDeviceStr(qemuCaps->binary,
                                    qemuCaps, runUid, runGid) < 0) {
        goto cleanup;
    }

    if (virQEMUCapsProbeCPUModels(qemuCaps, runUid, runGid) < 0)
        goto cleanup;

    if (virQEMUCapsProbeMachineTypes(qemuCaps, runUid, runGid) < 0)
        goto cleanup;

    ret = 0;
cleanup:
    virCommandFree(cmd);
    VIR_FREE(help);
    return ret;
}


static void virQEMUCapsMonitorNotify(qemuMonitorPtr mon ATTRIBUTE_UNUSED,
                                     virDomainObjPtr vm ATTRIBUTE_UNUSED)
{
}

static qemuMonitorCallbacks callbacks = {
    .eofNotify = virQEMUCapsMonitorNotify,
    .errorNotify = virQEMUCapsMonitorNotify,
};


/* Capabilities that we assume are always enabled
 * for QEMU >= 1.2.0
 */
static void
virQEMUCapsInitQMPBasic(virQEMUCapsPtr qemuCaps)
{
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_VNC_COLON);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_NO_REBOOT);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_NAME);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_UUID);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_VNET_HDR);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_TCP);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_EXEC);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_CACHE_V2);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_FORMAT);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_VGA);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_0_10);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_MEM_PATH);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_SERIAL);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_UNIX);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_CHARDEV);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_MONITOR_JSON);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_BALLOON);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DEVICE);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_SDL);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_SMP_TOPOLOGY);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_NETDEV);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_RTC);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_VHOST_NET);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_NO_HPET);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_NODEFCONFIG);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_BOOT_MENU);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_FSDEV);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_NAME_PROCESS);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_READONLY);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_SMBIOS_TYPE);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_VGA_NONE);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_MIGRATE_QEMU_FD);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_AIO);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_CHARDEV_SPICEVMC);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DEVICE_QXL_VGA);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_CACHE_DIRECTSYNC);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_NO_SHUTDOWN);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_CACHE_UNSAFE);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_FSDEV_READONLY);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_VIRTIO_BLK_SG_IO);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_COPY_ON_READ);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_CPU_HOST);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_FSDEV_WRITEOUT);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_DRIVE_IOTUNE);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_WAKEUP);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_NO_USER_CONFIG);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_NETDEV_BRIDGE);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_SECCOMP_SANDBOX);
    virQEMUCapsSet(qemuCaps, QEMU_CAPS_NO_KVM_PIT);
}


static int
virQEMUCapsInitQMP(virQEMUCapsPtr qemuCaps,
                   const char *libDir,
                   uid_t runUid,
                   gid_t runGid)
{
    int ret = -1;
    virCommandPtr cmd = NULL;
    qemuMonitorPtr mon = NULL;
    int major, minor, micro;
    char *package = NULL;
    int status = 0;
    virDomainChrSourceDef config;
    char *monarg = NULL;
    char *monpath = NULL;
    char *pidfile = NULL;
    char *archstr;
    pid_t pid = 0;
    virDomainObj vm;

    /* the ".sock" sufix is important to avoid a possible clash with a qemu
     * domain called "capabilities"
     */
    if (virAsprintf(&monpath, "%s/%s", libDir, "capabilities.monitor.sock") < 0) {
        virReportOOMError();
        goto cleanup;
    }
    if (virAsprintf(&monarg, "unix:%s,server,nowait", monpath) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    /* ".pidfile" suffix is used rather than ".pid" to avoid a possible clash
     * with a qemu domain called "capabilities"
     * Normally we'd use runDir for pid files, but because we're using
     * -daemonize we need QEMU to be allowed to create them, rather
     * than libvirtd. So we're using libDir which QEMU can write to
     */
    if (virAsprintf(&pidfile, "%s/%s", libDir, "capabilities.pidfile") < 0) {
        virReportOOMError();
        goto cleanup;
    }

    memset(&config, 0, sizeof(config));
    config.type = VIR_DOMAIN_CHR_TYPE_UNIX;
    config.data.nix.path = monpath;
    config.data.nix.listen = false;

    VIR_DEBUG("Try to get caps via QMP qemuCaps=%p", qemuCaps);

    /*
     * We explicitly need to use -daemonize here, rather than
     * virCommandDaemonize, because we need to synchronize
     * with QEMU creating its monitor socket API. Using
     * daemonize guarantees control won't return to libvirt
     * until the socket is present.
     */
    cmd = virCommandNewArgList(qemuCaps->binary,
                               "-S",
                               "-no-user-config",
                               "-nodefaults",
                               "-nographic",
                               "-M", "none",
                               "-qmp", monarg,
                               "-pidfile", pidfile,
                               "-daemonize",
                               NULL);
    virCommandAddEnvPassCommon(cmd);
    virCommandClearCaps(cmd);
    virCommandSetGID(cmd, runGid);
    virCommandSetUID(cmd, runUid);

    if (virCommandRun(cmd, &status) < 0)
        goto cleanup;

    if (status != 0) {
        ret = 0;
        VIR_DEBUG("QEMU %s exited with status %d", qemuCaps->binary, status);
        goto cleanup;
    }

    if (virPidFileReadPath(pidfile, &pid) < 0) {
        VIR_DEBUG("Failed to read pidfile %s", pidfile);
        ret = 0;
        goto cleanup;
    }

    memset(&vm, 0, sizeof(vm));
    vm.pid = pid;

    if (!(mon = qemuMonitorOpen(&vm, &config, true, &callbacks))) {
        ret = 0;
        goto cleanup;
    }

    virObjectLock(mon);

    if (qemuMonitorSetCapabilities(mon) < 0) {
        virErrorPtr err = virGetLastError();
        VIR_DEBUG("Failed to set monitor capabilities %s",
                  err ? err->message : "<unknown problem>");
        ret = 0;
        goto cleanup;
    }

    if (qemuMonitorGetVersion(mon,
                              &major, &minor, &micro,
                              &package) < 0) {
        virErrorPtr err = virGetLastError();
        VIR_DEBUG("Failed to query monitor version %s",
                  err ? err->message : "<unknown problem>");
        ret = 0;
        goto cleanup;
    }

    VIR_DEBUG("Got version %d.%d.%d (%s)",
              major, minor, micro, NULLSTR(package));

    if (major < 1 || (major == 1 && minor < 2)) {
        VIR_DEBUG("Not new enough for QMP capabilities detection");
        ret = 0;
        goto cleanup;
    }

    qemuCaps->version = major * 1000000 + minor * 1000 + micro;
    qemuCaps->usedQMP = true;

    virQEMUCapsInitQMPBasic(qemuCaps);

    if (!(archstr = qemuMonitorGetTargetArch(mon)))
        goto cleanup;

    if ((qemuCaps->arch = virQEMUCapsArchFromString(archstr)) == VIR_ARCH_NONE) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unknown QEMU arch %s"), archstr);
        VIR_FREE(archstr);
        goto cleanup;
    }
    VIR_FREE(archstr);

    /* Currently only x86_64 and i686 support PCI-multibus and -no-acpi. */
    if (qemuCaps->arch == VIR_ARCH_X86_64 ||
        qemuCaps->arch == VIR_ARCH_I686) {
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_PCI_MULTIBUS);
        virQEMUCapsSet(qemuCaps, QEMU_CAPS_NO_ACPI);
    }

    if (virQEMUCapsProbeQMPCommands(qemuCaps, mon) < 0)
        goto cleanup;
    if (virQEMUCapsProbeQMPEvents(qemuCaps, mon) < 0)
        goto cleanup;
    if (virQEMUCapsProbeQMPObjects(qemuCaps, mon) < 0)
        goto cleanup;
    if (virQEMUCapsProbeQMPMachineTypes(qemuCaps, mon) < 0)
        goto cleanup;
    if (virQEMUCapsProbeQMPCPUDefinitions(qemuCaps, mon) < 0)
        goto cleanup;
    if (virQEMUCapsProbeQMPKVMState(qemuCaps, mon) < 0)
        goto cleanup;

    ret = 0;

cleanup:
    if (mon)
        virObjectUnlock(mon);
    qemuMonitorClose(mon);
    virCommandAbort(cmd);
    virCommandFree(cmd);
    VIR_FREE(monarg);
    VIR_FREE(monpath);
    VIR_FREE(package);

    if (pid != 0) {
        char ebuf[1024];

        VIR_DEBUG("Killing QMP caps process %lld", (long long) pid);
        if (virProcessKill(pid, SIGKILL) < 0 && errno != ESRCH)
            VIR_ERROR(_("Failed to kill process %lld: %s"),
                      (long long) pid,
                      virStrerror(errno, ebuf, sizeof(ebuf)));
    }
    if (pidfile) {
        unlink(pidfile);
        VIR_FREE(pidfile);
    }
    return ret;
}


virQEMUCapsPtr virQEMUCapsNewForBinary(const char *binary,
                                       const char *libDir,
                                       uid_t runUid,
                                       gid_t runGid)
{
    virQEMUCapsPtr qemuCaps = virQEMUCapsNew();
    struct stat sb;
    int rv;

    if (!(qemuCaps->binary = strdup(binary)))
        goto no_memory;

    /* We would also want to check faccessat if we cared about ACLs,
     * but we don't.  */
    if (stat(binary, &sb) < 0) {
        virReportSystemError(errno, _("Cannot check QEMU binary %s"),
                             binary);
        goto error;
    }
    qemuCaps->mtime = sb.st_mtime;

    /* Make sure the binary we are about to try exec'ing exists.
     * Technically we could catch the exec() failure, but that's
     * in a sub-process so it's hard to feed back a useful error.
     */
    if (!virFileIsExecutable(binary)) {
        virReportSystemError(errno, _("QEMU binary %s is not executable"),
                             binary);
        goto error;
    }

    if ((rv = virQEMUCapsInitQMP(qemuCaps, libDir, runUid, runGid)) < 0)
        goto error;

    if (!qemuCaps->usedQMP &&
        virQEMUCapsInitHelp(qemuCaps, runUid, runGid) < 0)
        goto error;

    return qemuCaps;

no_memory:
    virReportOOMError();
error:
    virObjectUnref(qemuCaps);
    qemuCaps = NULL;
    return NULL;
}


bool virQEMUCapsIsValid(virQEMUCapsPtr qemuCaps)
{
    struct stat sb;

    if (!qemuCaps->binary)
        return true;

    if (stat(qemuCaps->binary, &sb) < 0)
        return false;

    return sb.st_mtime == qemuCaps->mtime;
}


static void
virQEMUCapsHashDataFree(void *payload, const void *key ATTRIBUTE_UNUSED)
{
    virObjectUnref(payload);
}


virQEMUCapsCachePtr
virQEMUCapsCacheNew(const char *libDir,
                    uid_t runUid,
                    gid_t runGid)
{
    virQEMUCapsCachePtr cache;

    if (VIR_ALLOC(cache) < 0) {
        virReportOOMError();
        return NULL;
    }

    if (virMutexInit(&cache->lock) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Unable to initialize mutex"));
        VIR_FREE(cache);
        return NULL;
    }

    if (!(cache->binaries = virHashCreate(10, virQEMUCapsHashDataFree)))
        goto error;
    if (!(cache->libDir = strdup(libDir))) {
        virReportOOMError();
        goto error;
    }

    cache->runUid = runUid;
    cache->runGid = runGid;

    return cache;

error:
    virQEMUCapsCacheFree(cache);
    return NULL;
}


virQEMUCapsPtr
virQEMUCapsCacheLookup(virQEMUCapsCachePtr cache, const char *binary)
{
    virQEMUCapsPtr ret = NULL;
    virMutexLock(&cache->lock);
    ret = virHashLookup(cache->binaries, binary);
    if (ret &&
        !virQEMUCapsIsValid(ret)) {
        VIR_DEBUG("Cached capabilities %p no longer valid for %s",
                  ret, binary);
        virHashRemoveEntry(cache->binaries, binary);
        ret = NULL;
    }
    if (!ret) {
        VIR_DEBUG("Creating capabilities for %s",
                  binary);
        ret = virQEMUCapsNewForBinary(binary, cache->libDir,
                                      cache->runUid, cache->runGid);
        if (ret) {
            VIR_DEBUG("Caching capabilities %p for %s",
                      ret, binary);
            if (virHashAddEntry(cache->binaries, binary, ret) < 0) {
                virObjectUnref(ret);
                ret = NULL;
            }
        }
    }
    VIR_DEBUG("Returning caps %p for %s", ret, binary);
    virObjectRef(ret);
    virMutexUnlock(&cache->lock);
    return ret;
}


virQEMUCapsPtr
virQEMUCapsCacheLookupCopy(virQEMUCapsCachePtr cache, const char *binary)
{
    virQEMUCapsPtr qemuCaps = virQEMUCapsCacheLookup(cache, binary);
    virQEMUCapsPtr ret;

    if (!qemuCaps)
        return NULL;

    ret = virQEMUCapsNewCopy(qemuCaps);
    virObjectUnref(qemuCaps);
    return ret;
}


void
virQEMUCapsCacheFree(virQEMUCapsCachePtr cache)
{
    if (!cache)
        return;

    VIR_FREE(cache->libDir);
    virHashFree(cache->binaries);
    virMutexDestroy(&cache->lock);
    VIR_FREE(cache);
}

bool
virQEMUCapsUsedQMP(virQEMUCapsPtr qemuCaps)
{
    return qemuCaps->usedQMP;
}
