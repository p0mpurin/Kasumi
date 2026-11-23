#include "xcloud_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

static void test_gs_base(void)
{
    CHECK(xcloud_valid_gs_base("https://uks.gssv-play-prod.xboxlive.com"));
    CHECK(xcloud_valid_gs_base("https://uks.core.gssv-play-prod.xboxlive.com"));
    CHECK(xcloud_valid_gs_base("https://uks.core.gssv-play-prod.xboxlive.com/"));
    CHECK(xcloud_valid_gs_base("https://xhome.gssv-play-prod.xboxlive.com"));
    CHECK(xcloud_valid_gs_base("https://uks.core.gssv-play-prodxhome.xboxlive.com"));
    CHECK(xcloud_valid_gs_base("https://brs.core.gssv-play-prodxhome.xboxlive.com/"));
    CHECK(!xcloud_valid_gs_base("http://uks.core.gssv-play-prod.xboxlive.com"));
    CHECK(!xcloud_valid_gs_base("http://uks.core.gssv-play-prodxhome.xboxlive.com"));
    CHECK(!xcloud_valid_gs_base("https://uks.core.gssv-play-prod.xboxlive.com/path"));
    CHECK(!xcloud_valid_gs_base("https://uks.core.gssv-play-prodxhome.xboxlive.com/path"));
    CHECK(!xcloud_valid_gs_base("https://evil.com/uks.gssv-play-prod.xboxlive.com"));
    CHECK(!xcloud_valid_gs_base("https://evil.com/uks.gssv-play-prodxhome.xboxlive.com"));
    CHECK(!xcloud_valid_gs_base("https://.gssv-play-prod.xboxlive.com"));
    CHECK(!xcloud_valid_gs_base("https://.gssv-play-prodxhome.xboxlive.com"));
    CHECK(!xcloud_valid_gs_base(""));
    CHECK(!xcloud_valid_gs_base(NULL));
}

static void test_subpath(void)
{
    char out[128];
    xcloud_session_subpath(out, sizeof(out), "v5/sessions/home/abc", "state");
    CHECK(!strcmp(out, "/v5/sessions/home/abc/state"));
    xcloud_session_subpath(out, sizeof(out), "/v5/sessions/home/abc", "/sdp");
    CHECK(!strcmp(out, "/v5/sessions/home/abc/sdp"));
    xcloud_session_subpath(out, sizeof(out), "/v5/sessions/home/abc", NULL);
    CHECK(!strcmp(out, "/v5/sessions/home/abc"));
}

static void test_ipv6_and_teredo(void)
{
    uint8_t b[16];
    CHECK(xcloud_parse_ipv6("fe80::", b));
    CHECK(b[0] == 0xfe && b[1] == 0x80 && b[15] == 0x00);
    CHECK(xcloud_parse_ipv6("::", b));
    CHECK(b[0] == 0 && b[15] == 0);
    CHECK(!xcloud_parse_ipv6("2001:0:::", b));
    CHECK(!xcloud_parse_ipv6("fe80::1::2", b));

    char ip4[32] = "";
    uint16_t port = 0;

    /* RFC 4380 Teredo example */
    CHECK(xcloud_teredo_ipv4("2001:0000:4136:e378:8000:63bf:3fff:fdd2", ip4, sizeof(ip4), &port));
    CHECK(!strcmp(ip4, "192.0.2.45"));
    CHECK(port == 40000);

    /* xbox-xcloud-player test vector */
    CHECK(xcloud_teredo_ipv4("2001::ce49:7601:e866:efff:62c3:fffe", ip4, sizeof(ip4), &port));
    CHECK(!strcmp(ip4, "157.60.0.1"));
    CHECK(port == 4096);

    /* Compressed :: form */
    CHECK(xcloud_teredo_ipv4("2001:0::8000:dbff:ff57:fefe", ip4, sizeof(ip4), &port));
    CHECK(!strcmp(ip4, "0.168.1.1"));
    CHECK(port == 9216);

    /* Non-Teredo IPv6 rejected */
    CHECK(!xcloud_teredo_ipv4("fe80::1", ip4, sizeof(ip4), &port));
    CHECK(!xcloud_teredo_ipv4("2606:4700:4700::1111", ip4, sizeof(ip4), &port));
    CHECK(!xcloud_teredo_ipv4("192.168.1.10", ip4, sizeof(ip4), &port));
    CHECK(!xcloud_teredo_ipv4("2001:0:::1", ip4, sizeof(ip4), &port));
}

static void test_home_store_and_power(void)
{
    CHECK(xcloud_is_home_store("Remote Play"));
    CHECK(!xcloud_is_home_store("Xbox"));
    CHECK(!xcloud_is_home_store(NULL));

    CHECK(xcloud_console_power("On") == XBC_POWER_ON);
    CHECK(xcloud_console_power("ConnectedStandby") == XBC_POWER_STANDBY);
    CHECK(xcloud_console_power("Off") == XBC_POWER_OFF);
    CHECK(xcloud_console_power("UnknownState") == XBC_POWER_UNKNOWN);
    CHECK(xcloud_console_power(NULL) == XBC_POWER_UNKNOWN);
}

int main(void)
{
    test_gs_base();
    test_subpath();
    test_ipv6_and_teredo();
    test_home_store_and_power();
    printf("PASS: gs_base, session_subpath, ipv6/teredo_ipv4, home_store, console_power\n");
    return 0;
}
