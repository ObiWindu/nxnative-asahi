#include "graphics/vulkan/nx_vk.h"
#include "memory/nx_mem.h"
#include "platform/linux/nx_platform.h"
#ifdef NXNATIVE_HAS_PROFILE
#include "game_profile.h"
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef NXNATIVE_BUILD_HOST
int dm_main(int argc, char **argv);
#else
int dm_main(int argc, char **argv) {
    (void)argc; (void)argv;
    fprintf(stderr, "this build has no guest host: configure with -DNXNATIVE_BUILD_HOST=ON -DNXNATIVE_GAME=<title id>\n");
    return 2;
}
#endif

static void usage(const char *argv0) {
    printf("nxnative-asahi — native Nintendo Switch runtime for Asahi Linux\n"
           "\n"
           "usage: %s <command> [options]\n"
           "\n"
           "commands:\n"
           "  sysinfo              print host, page-size and memory information\n"
           "  vk-info              initialize Vulkan and print the capability matrix\n"
           "  run <game-dir>       run a prepared game directory (host build required)\n"
           "  version              print the version\n"
           "  profile              print the compiled profile (title ID and name)\n"
           "  help                 this text\n"
           "\n"
           "environment:\n"
           "  NX_VK_DEVICE=<text>   select a Vulkan device by name substring\n"
           "  NX_VK_VALIDATION=1    enable Vulkan validation layers\n"
           "  NX_VK_ALLOW_CPU=1     allow llvmpipe (diagnostics only)\n"
           "  NX_LOG=<0..3>         log level (error..debug)\n",
           argv0 ? argv0 : "nxnative-asahi");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 2; }
    nx_log_set_level(nx_env_int("NX_LOG", NX_LOG_INFO));
    if (!strcmp(argv[1], "help") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) { usage(argv[0]); return 0; }
    if (!strcmp(argv[1], "version") || !strcmp(argv[1], "--version")) { printf("nxnative-asahi 0.1.0 (Asahi Linux)\n"); return 0; }
#ifdef NXNATIVE_HAS_PROFILE
    if (!strcmp(argv[1], "profile")) { printf("%s\t%s\n", DM_GAME_ID, DM_GAME_NAME); return 0; }
#endif
    if (!strcmp(argv[1], "sysinfo")) {
        NxSysInfo si;
        nx_sysinfo_query(&si);
        nx_sysinfo_print(&si);
        nx_mem_report();
        printf("\nAsahi page-size rule: guest pages are %zu bytes, host pages are %zu bytes; permissions and dirty tracking are grouped at host granularity.\n",
               nx_guest_page_size(), nx_host_page_size());
        return 0;
    }
    if (!strcmp(argv[1], "vk-info")) {
        NxVk vk;
        NxVkConfig cfg = {0};
        cfg.app_name = "nxnative-asahi";
        cfg.app_version = 1;
        cfg.enable_validation = nx_env_bool("NX_VK_VALIDATION", 0);
        cfg.allow_cpu = nx_env_bool("NX_VK_ALLOW_CPU", 0);
        cfg.device_filter = nx_env("NX_VK_DEVICE");
        if (nx_vk_init(&vk, &cfg) != 0) return 1;
        nx_vk_report(&vk, stdout);
        nx_vk_shutdown(&vk);
        return 0;
    }
    if (!strcmp(argv[1], "run")) {
        if (argc < 3) { NX_LOGE("run needs a prepared game directory"); return 2; }
        /* The host takes argv[0] as the program name and argv[1] as the module
           directory; pass any guest arguments through after it. */
        int hargc = argc - 1;
        char **hargv = calloc((size_t)hargc + 1, sizeof *hargv);
        if (!hargv) return 1;
        hargv[0] = argv[0];
        for (int i = 2; i < argc; i++) hargv[i - 1] = argv[i];
        int rc = dm_main(hargc, hargv);
        free(hargv);
        return rc;
    }
    NX_LOGE("unknown command: %s", argv[1]);
    usage(argv[0]);
    return 2;
}
