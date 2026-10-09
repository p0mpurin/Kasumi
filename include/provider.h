#pragma once

#include <stdbool.h>
#include <stddef.h>

/* GeForce NOW providers. In some countries GeForce NOW is run by an Alliance
 * partner (au in Japan, Taiwan Mobile, GFN Korea, Zain, GAME+...), with its
 * own sign-in (idp) and its own session service. NVIDIA publishes the list at
 * pcs.geforcenow.com/v1/serviceUrls, with its recommendation for the country
 * the request comes from. OpenNOW desktop and OpenNOW Vita use the same list:
 * the device sign-in on login.nvidia.com takes the partner's idp_id, and
 * serverInfo, the library's VPC and sessions use the partner's streaming URL.
 *
 * The provider belongs to the saved login: an account signed in through a
 * partner keeps using that partner until it signs out. */

#define PROVIDER_MAX 24
#define PROVIDER_NVIDIA "NVIDIA"
#define PROVIDER_NVIDIA_IDP "PDiAhv2kJTFeQ7WOPqiQ2tRZ7lGhR2X11dXvM4TZSxg"
#define PROVIDER_NVIDIA_URL "https://prod.cloudmatchbeta.nvidiagrid.net"
/* Xbox Cloud Gaming (xcloud.h) is a separate service, not in the list; this
 * names its sign-in on the login screen. */
#define PROVIDER_XBOX "XBOX"
#define PROVIDER_XBOX_URL "https://xgpuweb.gssv-play-prod.xboxlive.com"
/* Steam Link (steam_link.h): the player's own PC, paired on the local network. */
#define PROVIDER_STEAM "STEAM"

typedef struct {
    char code[12];
    char name[40];
    char idp[64];
    char url[96]; /* streaming service, https, no trailing slash */
} GfnProvider;

void provider_nvidia(GfnProvider *out);
void provider_xbox(GfnProvider *out);

/* The cached list from the SD card (startup). */
void providers_load(void);
/* Worker thread: fetch NVIDIA's list and recommendation; false if offline. */
bool providers_fetch(void);
unsigned providers_count(void);
bool providers_get(unsigned index, GfnProvider *out);
bool providers_find(const char *code, GfnProvider *out);
/* NVIDIA's recommendation for this country (NVIDIA when unknown). */
void providers_recommended(GfnProvider *out);
/* The country NVIDIA sees from this console's internet address ("AU"),
 * and its name for messages ("Australia", or the code itself). */
void providers_country(char *out, size_t size);
const char *providers_country_name(const char *code);
/* A partner runs GeForce NOW here (true: fills it in). `only` is set when
 * NVIDIA doesn't serve this country itself (Australia), so most accounts
 * there are the partner's; otherwise both sell it. */
bool providers_partner_here(GfnProvider *partner, bool *only);

/* The provider of the signed-in account (NVIDIA when none is saved). */
void provider_set_active(const GfnProvider *provider);
void provider_active(GfnProvider *out);
/* The active provider's session service URL (no trailing slash). */
void provider_base_url(char *out, size_t size);
bool provider_is_nvidia(void);
