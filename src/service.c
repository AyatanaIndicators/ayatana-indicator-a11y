/*
 * Copyright 2023-2024 Robert Tari <robert@tari.in>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <glib/gi18n.h>
#include <gio/gio.h>
#include <act/act.h>
#include <pwd.h>
#include <math.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <ayatana/common/utils.h>
#include "service.h"

#define BUS_NAME "org.ayatana.indicator.a11y"
#define BUS_PATH "/org/ayatana/indicator/a11y"
#define GREETER_BUS_NAME "org.ayatana.greeter"
#define GREETER_BUS_PATH "/org/ayatana/greeter"
#define GREETER_SETTINGS "org.ArcticaProject.arctica-greeter"

static guint m_nSignal = 0;

struct _IndicatorA11yServicePrivate
{
    guint nOwnId;
    guint nActionsId;
    GDBusConnection *pConnection;
    GSimpleActionGroup *pActionGroup;
    GMenu *pMenu;
    GMenu *pSubmenu;
    guint nExportId;
    GSimpleAction *pHeaderAction;
    guint nOnboardSubscription;
    gboolean bOnboardActive;
    GSettings *pOrcaSettings;
    guint nOrcaSubscription;
    gboolean bOrcaActive;
    gboolean bHighContrast;
    GSettings *pHighContrastSettings;
    gboolean bIgnoreSettings;
    gchar *sThemeGtk;
    gchar *sThemeIcon;
    gboolean bGreeter;
    GSList *lUsers;
    gchar *sUser;
    guint nUserSubscription;
    gboolean bReadingAccountsService;
    GDBusConnection *pAccountsServiceConnection;
    GSettings *pSettings;
    gboolean bMagnifierActive;
    gchar *sMagnifier;
    GPid nMagnifier;
    gdouble fScale;
    GSettings *pBackgroundSettings;
    gchar *sHighContrast;
    GSettings *pKeybindingSettings;
    GSettings *pApplicationsSettings;
    gboolean bScalingUnsupported;
};

typedef IndicatorA11yServicePrivate priv_t;

G_DEFINE_TYPE_WITH_PRIVATE (IndicatorA11yService, indicator_a11y_service, G_TYPE_OBJECT)

static GVariant* createHeaderState (IndicatorA11yService *self)
{
    GVariantBuilder cBuilder;
    g_variant_builder_init (&cBuilder, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&cBuilder, "{sv}", "title", g_variant_new_string (_("Accessibility")));
    g_variant_builder_add (&cBuilder, "{sv}", "tooltip", g_variant_new_string (_("Accessibility settings")));
    /* a11y indicator is not usable in Lomiri, so let's hide it when running in Lomiri */
    if (ayatana_common_utils_is_lomiri()) {
        g_variant_builder_add (&cBuilder, "{sv}", "visible", g_variant_new_boolean (FALSE));
    }
    else
    {
        g_variant_builder_add (&cBuilder, "{sv}", "visible", g_variant_new_boolean (TRUE));
    }
    g_variant_builder_add (&cBuilder, "{sv}", "accessible-desc", g_variant_new_string (_("Accessibility settings")));

    GIcon *pIcon = g_themed_icon_new_with_default_fallbacks ("preferences-desktop-accessibility-panel");
    GVariant *pSerialized = g_icon_serialize (pIcon);

    if (pSerialized != NULL)
    {
        g_variant_builder_add (&cBuilder, "{sv}", "icon", pSerialized);
        g_variant_unref (pSerialized);
    }

    g_object_unref (pIcon);

    return g_variant_builder_end (&cBuilder);
}

static void toggleScreensaverOnboard (gboolean bActive)
{
    GSettingsSchemaSource *pSource = g_settings_schema_source_get_default ();

    if (pSource)
    {
        GSettingsSchema *pSchema = g_settings_schema_source_lookup (pSource, "org.mate.screensaver", FALSE);

        if (pSchema)
        {
            g_settings_schema_unref (pSchema);
            GSettings *pSettings = g_settings_new ("org.mate.screensaver");
            g_settings_set_boolean (pSettings, "embedded-keyboard-enabled", bActive);
            g_clear_object (&pSettings);
        }
        else
        {
            g_warning ("Panic: No org.mate.screensaver schema found");
        }
    }
}

static void onOnboardBus (GDBusConnection *pConnection, const gchar *sSender, const gchar *sPath, const gchar *sInterface, const gchar *sSignal, GVariant *pParameters, gpointer pUserData)
{
    GVariant *pDict = g_variant_get_child_value (pParameters, 1);
    GVariant* pValue = g_variant_lookup_value (pDict, "Visible", G_VARIANT_TYPE_BOOLEAN);
    g_variant_unref (pDict);

    IndicatorA11yService *self = INDICATOR_A11Y_SERVICE (pUserData);
    gboolean bActive = g_variant_get_boolean (pValue);

    if (bActive != self->pPrivate->bOnboardActive)
    {
        toggleScreensaverOnboard (bActive);
        self->pPrivate->bOnboardActive = bActive;
        GAction *pAction = g_action_map_lookup_action (G_ACTION_MAP (self->pPrivate->pActionGroup), "onboard");
        g_action_change_state (pAction, pValue);
    }

    g_variant_unref (pValue);
}

static void getAccountsService (IndicatorA11yService *self, gint nUid)
{
    self->pPrivate->bReadingAccountsService = TRUE;
    gchar *sPath = g_strdup_printf ("/org/freedesktop/Accounts/User%i", nUid);
    GDBusProxy *pProxy = g_dbus_proxy_new_sync (self->pPrivate->pAccountsServiceConnection, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.freedesktop.Accounts", sPath, "org.freedesktop.DBus.Properties", NULL, NULL);
    g_free (sPath);

    if (pProxy)
    {
        const gchar *lProperties[] = {"orca", "onboard", "contrast", "magnifier"};

        for (gint nIndex = 0; nIndex < 4; nIndex++)
        {
            GVariant *pParams = g_variant_new ("(ss)", "org.ayatana.indicator.a11y.AccountsService", lProperties[nIndex]);
            GVariant *pValue = g_dbus_proxy_call_sync (pProxy, "Get", pParams, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);

            if (pValue)
            {
                GVariant *pChild0 = g_variant_get_child_value (pValue, 0);
                g_variant_unref (pValue);
                GVariant *pChild1 = g_variant_get_child_value (pChild0, 0);
                g_variant_unref (pChild0);
                GAction *pAction = g_action_map_lookup_action (G_ACTION_MAP (self->pPrivate->pActionGroup), lProperties[nIndex]);
                g_action_change_state (pAction, pChild1);
                g_variant_unref (pChild1);
            }
        }
    }

    self->pPrivate->bReadingAccountsService = FALSE;
}

static void onUserLoaded (IndicatorA11yService *self, ActUser *pUser)
{
    g_signal_handlers_disconnect_by_func (G_OBJECT (pUser), G_CALLBACK (onUserLoaded), self);

    if (!self->pPrivate->sUser)
    {
        GError *pError = NULL;
        GVariant *pUser = g_dbus_connection_call_sync (self->pPrivate->pConnection, GREETER_BUS_NAME, GREETER_BUS_PATH, GREETER_BUS_NAME, "GetUser", NULL, G_VARIANT_TYPE ("(s)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &pError);

        if (pError)
        {
            g_debug ("Failed calling GetUser: %s", pError->message);
            g_error_free (pError);

            return;
        }

        g_variant_get (pUser, "(s)", &self->pPrivate->sUser);
    }

    gboolean bPrefix = g_str_has_prefix (self->pPrivate->sUser, "*");

    if (!bPrefix)
    {
        const gchar *sUser = act_user_get_user_name (pUser);
        gboolean bSame = g_str_equal (self->pPrivate->sUser, sUser);

        if (bSame)
        {
            gint nUid = act_user_get_uid (pUser);
            getAccountsService (self, nUid);
        }
    }
}

static void onManagerLoaded (IndicatorA11yService *self)
{
    ActUserManager *pManager = act_user_manager_get_default ();

    if (!self->pPrivate->lUsers)
    {
        self->pPrivate->lUsers = act_user_manager_list_users (pManager);
    }

    for (GSList *lUser = self->pPrivate->lUsers; lUser; lUser = lUser->next)
    {
        ActUser *pUser = lUser->data;
        gboolean bLoaded = act_user_is_loaded (pUser);

        if (bLoaded)
        {
            onUserLoaded (self, pUser);
        }
        else
        {
            g_signal_connect_swapped (pUser, "notify::is-loaded", G_CALLBACK (onUserLoaded), self);
        }
    }
}

static void loadManager (IndicatorA11yService *self)
{
    ActUserManager *pManager = act_user_manager_get_default ();
    gboolean bLoaded = FALSE;
    g_object_get (pManager, "is-loaded", &bLoaded, NULL);

    if (bLoaded)
    {
        onManagerLoaded (self);
    }
    else
    {
        g_signal_connect_object (pManager, "notify::is-loaded", G_CALLBACK (onManagerLoaded), self, G_CONNECT_SWAPPED);
    }
}

static void onUserChanged (GDBusConnection *pConnection, const gchar *sSender, const gchar *sPath, const gchar *sInterface, const gchar *sSignal, GVariant *pParameters, gpointer pUserData)
{
    IndicatorA11yService *self = INDICATOR_A11Y_SERVICE (pUserData);
    g_variant_get (pParameters, "(s)", &self->pPrivate->sUser);
    loadManager (self);
}

static void onBusAcquired (GDBusConnection *pConnection, const gchar *sName, gpointer pData)
{
    IndicatorA11yService *self = INDICATOR_A11Y_SERVICE (pData);

    g_debug ("bus acquired: %s", sName);

    GError *pError = NULL;
    self->pPrivate->nActionsId = g_dbus_connection_export_action_group (pConnection, BUS_PATH, G_ACTION_GROUP (self->pPrivate->pActionGroup), &pError);

    // Export the actions
    if (!self->pPrivate->nActionsId)
    {
        g_warning ("Cannot export action group: %s", pError->message);
        g_clear_error(&pError);
    }

    // Export the menu
    gchar *sPath = g_strdup_printf ("%s/desktop", BUS_PATH);
    self->pPrivate->nExportId = g_dbus_connection_export_menu_model (pConnection, sPath, G_MENU_MODEL (self->pPrivate->pMenu), &pError);

    if (!self->pPrivate->nExportId)
    {
        g_warning ("Cannot export %s menu: %s", sPath, pError->message);
        g_clear_error (&pError);
    }

    g_free (sPath);
}

static void unexport (IndicatorA11yService *self)
{
    // Unexport the menu
    if (self->pPrivate->nExportId)
    {
        g_dbus_connection_unexport_menu_model (self->pPrivate->pConnection, self->pPrivate->nExportId);
        self->pPrivate->nExportId = 0;
    }

    // Unexport the actions
    if (self->pPrivate->nActionsId)
    {
        g_dbus_connection_unexport_action_group (self->pPrivate->pConnection, self->pPrivate->nActionsId);
        self->pPrivate->nActionsId = 0;
    }
}

static void onNameLost (GDBusConnection *pConnection, const gchar *sName, gpointer pData)
{
    IndicatorA11yService *self = INDICATOR_A11Y_SERVICE (pData);

    g_debug ("%s %s name lost %s", G_STRLOC, G_STRFUNC, sName);

    unexport (self);
}

static void onDispose (GObject *pObject)
{
    IndicatorA11yService *self = INDICATOR_A11Y_SERVICE (pObject);

    if (self->pPrivate->nOnboardSubscription)
    {
        g_dbus_connection_signal_unsubscribe (self->pPrivate->pConnection, self->pPrivate->nOnboardSubscription);
    }

    g_clear_object (&self->pPrivate->pHighContrastSettings);
    g_clear_object (&self->pPrivate->pBackgroundSettings);

    if (self->pPrivate->sThemeGtk)
    {
        g_free (self->pPrivate->sThemeGtk);
    }

    if (self->pPrivate->sThemeIcon)
    {
        g_free (self->pPrivate->sThemeIcon);
    }

    if (self->pPrivate->sHighContrast)
    {
        g_free (self->pPrivate->sHighContrast);
    }

    if (self->pPrivate->sMagnifier)
    {
        g_free (self->pPrivate->sMagnifier);
    }

    if (self->pPrivate->nUserSubscription)
    {
        g_dbus_connection_signal_unsubscribe (self->pPrivate->pConnection, self->pPrivate->nUserSubscription);
    }

    if (self->pPrivate->lUsers)
    {
        g_slist_free (self->pPrivate->lUsers);
    }

    g_clear_object (&self->pPrivate->pOrcaSettings);

    if (self->pPrivate->nOwnId)
    {
        g_bus_unown_name (self->pPrivate->nOwnId);
        self->pPrivate->nOwnId = 0;
    }

    unexport (self);

    g_clear_object (&self->pPrivate->pApplicationsSettings);
    g_clear_object (&self->pPrivate->pKeybindingSettings);
    g_clear_object (&self->pPrivate->pSettings);
    g_clear_object (&self->pPrivate->pHeaderAction);
    g_clear_object (&self->pPrivate->pActionGroup);
    g_clear_object (&self->pPrivate->pConnection);
    g_clear_object (&self->pPrivate->pAccountsServiceConnection);

    G_OBJECT_CLASS (indicator_a11y_service_parent_class)->dispose (pObject);
}

static void setAccountsService (IndicatorA11yService *self, gchar *sProperty, GVariant *pValue)
{
    if (self->pPrivate->pAccountsServiceConnection)
    {
        gint nUid = 0;

        if (!self->pPrivate->bGreeter)
        {
            nUid = geteuid ();
        }
        else if (self->pPrivate->sUser)
        {
            const struct passwd *pPasswd = getpwnam (self->pPrivate->sUser);

            if (pPasswd)
            {
                nUid = pPasswd->pw_uid;
            }
        }

        if (nUid)
        {
            gchar *sPath = g_strdup_printf ("/org/freedesktop/Accounts/User%i", nUid);
            GDBusProxy *pProxy = g_dbus_proxy_new_sync (self->pPrivate->pAccountsServiceConnection, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.freedesktop.Accounts", sPath, "org.freedesktop.DBus.Properties", NULL, NULL);
            g_free (sPath);
            GVariant *pParams = g_variant_new ("(ssv)", "org.ayatana.indicator.a11y.AccountsService", sProperty, pValue);
            GVariant *pRet = g_dbus_proxy_call_sync (pProxy, "Set", pParams, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
            g_variant_unref (pRet);
        }
    }
}

static void onOnboardState (GSimpleAction *pAction, GVariant* pValue, gpointer pUserData)
{
    g_simple_action_set_state (pAction, pValue);

    gboolean bActive = g_variant_get_boolean (pValue);
    IndicatorA11yService *self = INDICATOR_A11Y_SERVICE (pUserData);

    if (bActive != self->pPrivate->bOnboardActive)
    {
        gchar *sFunction = NULL;

        if (bActive)
        {
            sFunction = "Show";
        }
        else
        {
            sFunction = "Hide";
        }

        GError *pError = NULL;

        if (!self->pPrivate->bGreeter)
        {
            g_dbus_connection_call_sync (self->pPrivate->pConnection, "org.onboard.Onboard", "/org/onboard/Onboard/Keyboard", "org.onboard.Onboard.Keyboard", sFunction, NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &pError);
            toggleScreensaverOnboard (bActive);
        }
        else
        {
            GVariant *pParam = g_variant_new ("(b)", bActive);
            g_dbus_connection_call_sync (self->pPrivate->pConnection, GREETER_BUS_NAME, GREETER_BUS_PATH, GREETER_BUS_NAME, "ToggleOnBoard", pParam, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &pError);
        }

        if (pError)
        {
            g_warning ("Panic: Failed to toggle Onboard: %s", pError->message);
            g_error_free (pError);

            return;
        }

        self->pPrivate->bOnboardActive = bActive;

        if (!self->pPrivate->bReadingAccountsService)
        {
            GVariant *pValue = g_variant_new ("b", bActive);
            setAccountsService (self, "onboard", pValue);
        }
    }
}

static void onMagnifierExit (GPid nPid, gint nStatus, gpointer pData)
{
    IndicatorA11yService *self = INDICATOR_A11Y_SERVICE (pData);
    self->pPrivate->bMagnifierActive = FALSE;
    self->pPrivate->nMagnifier = 0;
    GVariant *pActionValue = g_variant_new ("b", FALSE);
    GAction *pAction = g_action_map_lookup_action (G_ACTION_MAP (self->pPrivate->pActionGroup), "magnifier");
    g_simple_action_set_state (G_SIMPLE_ACTION (pAction), pActionValue);

    if (!self->pPrivate->bReadingAccountsService)
    {
        GVariant *pValue = g_variant_new ("b", FALSE);
        setAccountsService (self, "magnifier", pValue);
    }
}

static void onMagnifierState (GSimpleAction *pAction, GVariant* pValue, gpointer pUserData)
{
    IndicatorA11yService *self = INDICATOR_A11Y_SERVICE (pUserData);

    if (self->pPrivate->sMagnifier)
    {
        g_simple_action_set_state (pAction, pValue);
        gboolean bActive = g_variant_get_boolean (pValue);

        if (bActive != self->pPrivate->bMagnifierActive)
        {
            GError *pError = NULL;

            if (!self->pPrivate->bGreeter)
            {
                if (bActive)
                {
                    gboolean bFound = ayatana_common_utils_have_program (self->pPrivate->sMagnifier);

                    if (!bFound)
                    {
                        gchar *sMessage = g_strdup_printf (_("The %s program is required for this action, but it was not found."), self->pPrivate->sMagnifier);
                        ayatana_common_utils_zenity_warning ("dialog-warning", _("Warning"), sMessage);
                        g_free (sMessage);

                        return;
                    }
                    else
                    {
                        gchar *lParams[] = {self->pPrivate->sMagnifier, NULL};
                        g_spawn_async (NULL, lParams, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &self->pPrivate->nMagnifier, &pError);
                        g_child_watch_add (self->pPrivate->nMagnifier, onMagnifierExit, self);
                    }
                }
                else if (self->pPrivate->nMagnifier)
                {
                    kill (self->pPrivate->nMagnifier, SIGTERM);
                }
            }
            else
            {
                GVariant *pParam = g_variant_new ("(b)", bActive);
                g_dbus_connection_call_sync (self->pPrivate->pConnection, GREETER_BUS_NAME, GREETER_BUS_PATH, GREETER_BUS_NAME, "ToggleMagnifier", pParam, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &pError);
            }

            if (pError)
            {
                g_warning ("Panic: Failed to toggle magnifier: %s", pError->message);
                g_error_free (pError);

                return;
            }

            self->pPrivate->bMagnifierActive = bActive;

            if (!self->pPrivate->bReadingAccountsService)
            {
                GVariant *pValue = g_variant_new ("b", bActive);
                setAccountsService (self, "magnifier", pValue);
            }
        }
    }
}

static void onScaleState (gpointer pUserData)
{
    IndicatorA11yService *self = INDICATOR_A11Y_SERVICE (pUserData);

    if (self->pPrivate->pSettings)
    {
        gdouble fScale = g_settings_get_double (self->pPrivate->pSettings, "scale");

        if (fScale != self->pPrivate->fScale)
        {
            Display *pDisplay = XOpenDisplay (NULL);

            if (!pDisplay)
            {
                g_warning ("Panic: Failed to open X display while setting display scale");

                return;
            }

            XGrabServer (pDisplay);
            guint nScreen = DefaultScreen (pDisplay);
            Window pWindow = RootWindow (pDisplay, nScreen);
            XRRScreenResources *pResources = XRRGetScreenResources (pDisplay, pWindow);

            if (!pResources)
            {
                g_warning ("Panic: Failed to get screen resources while setting display scale");
                XCloseDisplay (pDisplay);

                return;
            }

            guint nScreenWidth = 0;
            guint nScreenHeight = 0;

            // Get the Dpi
            gint nDisplayHeight = DisplayHeight (pDisplay, nScreen);
            gint nDisplayHeightMetric = DisplayHeightMM (pDisplay, nScreen);
            gdouble fDpi = (25.4 * nDisplayHeight) / nDisplayHeightMetric;

            // Scale the primary display
            guint nPrimaryWidth = 0;
            guint nPrimaryHeight = 0;
            RROutput nOutputPrimary = XRRGetOutputPrimary (pDisplay, pWindow);
            XRROutputInfo *pOutputInfo = XRRGetOutputInfo (pDisplay, pResources, nOutputPrimary);

            if (pOutputInfo->connection == RR_Connected && pOutputInfo->crtc)
            {
                XRRCrtcInfo *pCrtcInfo = XRRGetCrtcInfo (pDisplay, pResources, pOutputInfo->crtc);

                if (!pCrtcInfo)
                {
                    g_warning ("Panic: Failed to get CRTC info for primary display");
                    XRRFreeOutputInfo (pOutputInfo);
                    XRRFreeScreenResources (pResources);
                    XCloseDisplay (pDisplay);

                    return;
                }

                XTransform cTransform;
                memset (&cTransform, 0, sizeof (cTransform));
                cTransform.matrix[0][0] = XDoubleToFixed (fScale);
                cTransform.matrix[1][1] = XDoubleToFixed (fScale);
                cTransform.matrix[2][2] = XDoubleToFixed (1.0);
                gchar *sFilter = NULL;

                if (fScale == 0.5 || fScale == 1.0 || fScale == 2.0)
                {
                    sFilter = "nearest";
                }
                else
                {
                    sFilter = "bilinear";
                }

                for (gint nMode = 0; nMode < pResources->nmode; nMode++)
                {
                    if (pCrtcInfo->mode == pResources->modes[nMode].id)
                    {
                        if (fScale > 1.0)
                        {
                            nPrimaryWidth = ceil (pResources->modes[nMode].width * fScale);
                            nPrimaryHeight = ceil (pResources->modes[nMode].height * fScale);
                        }
                        else
                        {
                            nPrimaryWidth = pResources->modes[nMode].width;
                            nPrimaryHeight = pResources->modes[nMode].height;
                        }

                        nScreenWidth = nPrimaryWidth;
                        nScreenHeight = nPrimaryHeight;

                        break;
                    }
                }

                XRRSetCrtcTransform (pDisplay, pOutputInfo->crtc, &cTransform, sFilter, NULL, 0);
                Status nStatus = XRRSetCrtcConfig (pDisplay, pResources, pOutputInfo->crtc, CurrentTime, pCrtcInfo->x, pCrtcInfo->y, pCrtcInfo->mode, pCrtcInfo->rotation, pCrtcInfo->outputs, pCrtcInfo->noutput);

                if (nStatus != RRSetConfigSuccess)
                {
                    g_warning ("Panic: Failed to set CRTC info for primary display");

                    XRRFreeCrtcInfo(pCrtcInfo);

                    return;
                }

                XRRFreeCrtcInfo(pCrtcInfo);
            }

            XRRFreeOutputInfo (pOutputInfo);

            for (gint nOutput = 0; nOutput < pResources->noutput; nOutput++)
            {
                XRROutputInfo *pOutputInfo = XRRGetOutputInfo (pDisplay, pResources, pResources->outputs[nOutput]);

                if (pOutputInfo->connection == RR_Connected && pOutputInfo->crtc)
                {
                    XRRCrtcInfo *pCrtcInfo = XRRGetCrtcInfo (pDisplay, pResources, pOutputInfo->crtc);

                    if (!pCrtcInfo)
                    {
                        g_warning ("Panic: Failed to get CRTC info while iterating displays");
                        XRRFreeOutputInfo (pOutputInfo);
                        XRRFreeScreenResources (pResources);
                        XCloseDisplay (pDisplay);

                        return;
                    }

                    if (pResources->outputs[nOutput] != nOutputPrimary)
                    {
                        gboolean bReposition = FALSE;

                        if (pCrtcInfo->x)
                        {
                            pCrtcInfo->x = nPrimaryWidth;
                            bReposition = TRUE;
                        }

                        if (pCrtcInfo->y)
                        {
                            pCrtcInfo->y = nPrimaryHeight;
                            bReposition = TRUE;
                        }

                        if (bReposition)
                        {
                            Status nStatus = XRRSetCrtcConfig (pDisplay, pResources, pOutputInfo->crtc, CurrentTime, pCrtcInfo->x, pCrtcInfo->y, pCrtcInfo->mode, pCrtcInfo->rotation, pCrtcInfo->outputs, pCrtcInfo->noutput);

                            if (nStatus != RRSetConfigSuccess)
                            {
                                g_warning ("Panic: Failed to set CRTC info for auxiliary display");

                                return;
                            }
                        }
                    }

                    nScreenWidth = MAX (nScreenWidth, pCrtcInfo->x + pCrtcInfo->width);
                    nScreenHeight = MAX (nScreenHeight, pCrtcInfo->y + pCrtcInfo->height);
                    XRRFreeCrtcInfo(pCrtcInfo);
                }

                XRRFreeOutputInfo (pOutputInfo);
            }

            g_debug ("Resizing screen to: %ix%i", nScreenWidth, nScreenHeight);
            XRRSetScreenSize (pDisplay, pWindow, nScreenWidth, nScreenHeight, (gint) ceil ((25.4 * nScreenWidth) / fDpi), (gint) ceil ((25.4 * nScreenHeight) / fDpi));
            XRRFreeScreenResources (pResources);
            XUngrabServer (pDisplay);
            XCloseDisplay (pDisplay);
            self->pPrivate->fScale = fScale;
        }
    }
}

static void onOrcaState (GSimpleAction *pAction, GVariant* pValue, gpointer pUserData)
{
    g_simple_action_set_state (pAction, pValue);

    IndicatorA11yService *self = INDICATOR_A11Y_SERVICE (pUserData);

    if (self->pPrivate->bGreeter)
    {
        gboolean bActive = g_variant_get_boolean (pValue);

        if (bActive != self->pPrivate->bOrcaActive)
        {
            GError *pError = NULL;
            GVariant *pParam = g_variant_new ("(b)", bActive);
            g_dbus_connection_call_sync (self->pPrivate->pConnection, GREETER_BUS_NAME, GREETER_BUS_PATH, GREETER_BUS_NAME, "ToggleOrca", pParam, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &pError);

            if (pError)
            {
                g_warning ("Panic: Failed to toggle Orca: %s", pError->message);
                g_error_free (pError);

                return;
            }

            self->pPrivate->bOrcaActive = bActive;

            if (!self->pPrivate->bReadingAccountsService)
            {
                GVariant *pValue = g_variant_new ("b", bActive);
                setAccountsService (self, "orca", pValue);
            }
        }
    }
}

static void onContrastState (GSimpleAction *pAction, GVariant* pValue, gpointer pUserData)
{
    IndicatorA11yService *self = INDICATOR_A11Y_SERVICE (pUserData);

    if (self->pPrivate->pHighContrastSettings && self->pPrivate->pBackgroundSettings && self->pPrivate->pSettings && self->pPrivate->sHighContrast && self->pPrivate->sThemeIcon && self->pPrivate->sThemeGtk)
    {
        g_simple_action_set_state (pAction, pValue);
        gboolean bActive = g_variant_get_boolean (pValue);

        if (bActive != self->pPrivate->bHighContrast)
        {
            self->pPrivate->bHighContrast = bActive;

            if (!self->pPrivate->bGreeter)
            {
                self->pPrivate->bIgnoreSettings = TRUE;

                if (bActive)
                {
                    g_free (self->pPrivate->sThemeGtk);
                    g_free (self->pPrivate->sThemeIcon);
                    self->pPrivate->sThemeGtk = g_settings_get_string (self->pPrivate->pHighContrastSettings, "gtk-theme");
                    self->pPrivate->sThemeIcon = g_settings_get_string (self->pPrivate->pHighContrastSettings, "icon-theme");
                    g_settings_set_string (self->pPrivate->pHighContrastSettings, "gtk-theme", self->pPrivate->sHighContrast);
                    g_settings_set_string (self->pPrivate->pHighContrastSettings, "icon-theme", "ContrastHigh");
                    g_settings_set_string (self->pPrivate->pSettings, "gtk-theme", self->pPrivate->sThemeGtk);
                    g_settings_set_string (self->pPrivate->pSettings, "icon-theme", self->pPrivate->sThemeIcon);

                    g_settings_set_string (self->pPrivate->pBackgroundSettings, "color-shading-type", "solid");
                    g_settings_set_string (self->pPrivate->pBackgroundSettings, "picture-filename", "");
                    g_settings_set_string (self->pPrivate->pBackgroundSettings, "picture-options", "wallpaper");

                    gboolean bInverse = g_str_equal (self->pPrivate->sHighContrast, "HighContrastInverse");

                    if (bInverse)
                    {
                        g_settings_set_string (self->pPrivate->pBackgroundSettings, "primary-color", "rgb(0,0,0)");
                    }
                    else
                    {
                        g_settings_set_string (self->pPrivate->pBackgroundSettings, "primary-color", "rgb(255,255,255)");
                    }
                }
                else
                {
                    g_settings_set_string (self->pPrivate->pHighContrastSettings, "gtk-theme", self->pPrivate->sThemeGtk);
                    g_settings_set_string (self->pPrivate->pHighContrastSettings, "icon-theme", self->pPrivate->sThemeIcon);

                    const gchar *lProperties[] = {"color-shading-type", "picture-filename", "picture-options", "primary-color"};

                    for (guint nProperty = 0; nProperty < 4; nProperty++)
                    {
                        gchar *sValue = g_settings_get_string (self->pPrivate->pSettings, lProperties[nProperty]);
                        g_settings_set_string (self->pPrivate->pBackgroundSettings, lProperties[nProperty], sValue);
                        g_free (sValue);
                    }
                }

                self->pPrivate->bIgnoreSettings = FALSE;
            }
            else
            {
                GError *pError = NULL;
                GVariant *pParam = g_variant_new ("(b)", bActive);
                g_dbus_connection_call_sync (self->pPrivate->pConnection, GREETER_BUS_NAME, GREETER_BUS_PATH, GREETER_BUS_NAME, "ToggleHighContrast", pParam, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &pError);

                if (pError)
                {
                    g_warning ("Panic: Failed to toggle high contrast: %s", pError->message);
                    g_error_free (pError);

                    return;
                }
            }

            if (!self->pPrivate->bReadingAccountsService)
            {
                GVariant *pValue = g_variant_new ("b", bActive);
                setAccountsService (self, "contrast", pValue);
            }
        }
    }
}

static void onBackgroundSettings (GSettings *pSettings, const gchar *sKey, gpointer pUserData)
{
    IndicatorA11yService *self = INDICATOR_A11Y_SERVICE (pUserData);

    if (self->pPrivate->pBackgroundSettings && self->pPrivate->pSettings)
    {
        if (!self->pPrivate->bHighContrast)
        {
            gchar *sValue = g_settings_get_string (self->pPrivate->pBackgroundSettings, sKey);
            g_settings_set_string (self->pPrivate->pSettings, sKey, sValue);
            g_free (sValue);
        }
    }
}

static void onContrastThemeSettings (GSettings *pSettings, const gchar *sKey, gpointer pUserData)
{
    IndicatorA11yService *self = INDICATOR_A11Y_SERVICE (pUserData);

    if (self->pPrivate->pHighContrastSettings && self->pPrivate->pBackgroundSettings && self->pPrivate->pSettings && self->pPrivate->sHighContrast)
    {
        if (self->pPrivate->sHighContrast)
        {
            g_free (self->pPrivate->sHighContrast);
        }

        self->pPrivate->sHighContrast = g_settings_get_string (self->pPrivate->pSettings, "high-contrast");

        if (self->pPrivate->bHighContrast)
        {
            self->pPrivate->bIgnoreSettings = TRUE;
            g_settings_set_string (self->pPrivate->pHighContrastSettings, "gtk-theme", self->pPrivate->sHighContrast);
            gboolean bInverse = g_str_equal (self->pPrivate->sHighContrast, "HighContrastInverse");

            if (bInverse)
            {
                g_settings_set_string (self->pPrivate->pBackgroundSettings, "primary-color", "rgb(0,0,0)");
            }
            else
            {
                g_settings_set_string (self->pPrivate->pBackgroundSettings, "primary-color", "rgb(255,255,255)");
            }

            self->pPrivate->bIgnoreSettings = FALSE;
        }
    }
}

static void onContrastSettings (GSettings *pSettings, const gchar *sKey, gpointer pUserData)
{
    IndicatorA11yService *self = INDICATOR_A11Y_SERVICE (pUserData);

    if (self->pPrivate->pHighContrastSettings && self->pPrivate->pSettings && self->pPrivate->sThemeIcon && self->pPrivate->sThemeGtk)
    {
        if (self->pPrivate->bIgnoreSettings)
        {
            return;
        }

        gboolean bThemeGtk = g_str_equal (sKey, "gtk-theme");
        gboolean bThemeIcon = g_str_equal (sKey, "icon-theme");

        if (bThemeGtk)
        {
            g_free (self->pPrivate->sThemeGtk);
            self->pPrivate->sThemeGtk = g_settings_get_string (self->pPrivate->pHighContrastSettings, "gtk-theme");
            g_settings_set_string (self->pPrivate->pSettings, "gtk-theme", self->pPrivate->sThemeGtk);
        }
        else if (bThemeIcon)
        {
            g_free (self->pPrivate->sThemeIcon);
            self->pPrivate->sThemeIcon = g_settings_get_string (self->pPrivate->pHighContrastSettings, "icon-theme");
            g_settings_set_string (self->pPrivate->pSettings, "icon-theme", self->pPrivate->sThemeIcon);
        }

        bThemeGtk = g_str_equal (self->pPrivate->sThemeGtk, "ContrastHigh") || g_str_equal (self->pPrivate->sThemeGtk, "HighContrastInverse");
        bThemeIcon = g_str_equal (self->pPrivate->sThemeIcon, "ContrastHigh");
        gboolean bHighContrast = (bThemeGtk && bThemeIcon);

        if (self->pPrivate->bHighContrast != bHighContrast)
        {
            GAction *pAction = g_action_map_lookup_action (G_ACTION_MAP (self->pPrivate->pActionGroup), "contrast");
            GVariant *pValue = g_variant_new_boolean (bHighContrast);
            g_action_change_state (pAction, pValue);
        }
    }
}

static gboolean valueFromVariant (GValue *pValue, GVariant *pVariant, gpointer pUserData)
{
    g_value_set_variant (pValue, pVariant);

    return TRUE;
}

static GVariant* valueToVariant (const GValue *pValue, const GVariantType *pType, gpointer pUserData)
{
    GVariant *pVariant = g_value_dup_variant (pValue);

    return pVariant;
}

static void setAccelerator (GMenuItem *pItem, gchar *sKey, IndicatorA11yService *self)
{
    if (self->pPrivate->pKeybindingSettings)
    {
        if (!self->pPrivate->bGreeter)
        {
            gchar *sAccelerator = NULL;
            gboolean bMate = ayatana_common_utils_is_mate ();

            if (bMate)
            {
                sAccelerator = g_settings_get_string (self->pPrivate->pKeybindingSettings, sKey);

                if (sAccelerator)
                {
                    g_menu_item_set_attribute (pItem, "accel", "s", sAccelerator);
                    g_free (sAccelerator);
                }
            }
            else
            {
                gchar **lAccelerators = g_settings_get_strv (self->pPrivate->pKeybindingSettings, sKey);

                if (lAccelerators)
                {
                    g_menu_item_set_attribute (pItem, "accel", "s", lAccelerators[0]);
                    g_strfreev (lAccelerators);
                }
            }
        }
    }
}

static void indicator_a11y_service_init (IndicatorA11yService *self)
{
    self->pPrivate = indicator_a11y_service_get_instance_private (self);
    const char *sUser = g_get_user_name();
    self->pPrivate->bGreeter = g_str_equal (sUser, "lightdm");
    self->pPrivate->sUser = NULL;
    self->pPrivate->bOnboardActive = FALSE;
    self->pPrivate->bOrcaActive = FALSE;
    self->pPrivate->bMagnifierActive = FALSE;
    self->pPrivate->fScale = 0;
    self->pPrivate->sMagnifier = NULL;
    self->pPrivate->nMagnifier = 0;
    self->pPrivate->sThemeGtk = NULL;
    self->pPrivate->sThemeIcon = NULL;
    self->pPrivate->bIgnoreSettings = FALSE;
    self->pPrivate->lUsers = NULL;
    self->pPrivate->sUser = NULL;
    self->pPrivate->bReadingAccountsService = FALSE;
    self->pPrivate->sHighContrast = NULL;
    self->pPrivate->bScalingUnsupported = FALSE;
    GError *pError = NULL;

    // Check if we are on Wayland
    const gchar *sWayland = g_getenv ("WAYLAND_DISPLAY");
    self->pPrivate->bScalingUnsupported = (sWayland != NULL);
    //~Check if we are on Wayland

    // Check if we are in a virtual environment
    if (!self->pPrivate->bScalingUnsupported)
    {
        Display *pDisplay = XOpenDisplay (NULL);

        if (!pDisplay)
        {
            g_warning ("Panic: Failed to open X display while checking for virtual environment");
        }
        else
        {
            guint nScreen = DefaultScreen (pDisplay);
            Window pWindow = RootWindow (pDisplay, nScreen);
            XRRScreenResources *pResources = XRRGetScreenResources (pDisplay, pWindow);

            if (!pResources)
            {
                g_warning ("Panic: Failed to get screen resources while checking for virtual environment");
                XCloseDisplay (pDisplay);
            }
            else
            {
                RROutput nOutputPrimary = XRRGetOutputPrimary (pDisplay, pWindow);
                XRROutputInfo *pOutputInfo = XRRGetOutputInfo (pDisplay, pResources, nOutputPrimary);
                GRegex *pRegex = NULL;

                #if GLIB_CHECK_VERSION(2, 73, 0)
                    pRegex = g_regex_new (".*virtual.*", G_REGEX_CASELESS, G_REGEX_MATCH_DEFAULT, &pError);
                #else
                    pRegex = g_regex_new (".*virtual.*", G_REGEX_CASELESS, (GRegexMatchFlags) 0, &pError);
                #endif

                if (!pError)
                {
                    #if GLIB_CHECK_VERSION(2, 73, 0)
                        gboolean bMatch = g_regex_match (pRegex, pOutputInfo->name, G_REGEX_MATCH_DEFAULT, NULL);
                    #else
                        gboolean bMatch = g_regex_match (pRegex, pOutputInfo->name, (GRegexMatchFlags) 0, NULL);
                    #endif

                    if (bMatch)
                    {
                        self->pPrivate->bScalingUnsupported = TRUE;
                    }

                    g_regex_unref (pRegex);
                }
                else
                {
                    g_warning ("Panic: Failed to compile regex: %s", pError->message);
                    g_error_free (pError);
                }

                XRRFreeOutputInfo (pOutputInfo);
                XRRFreeScreenResources (pResources);
                XCloseDisplay (pDisplay);
            }
        }
    }
    //~Check if we are in a virtual environment

    self->pPrivate->pAccountsServiceConnection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &pError);

    if (pError)
    {
        g_warning ("Panic: Failed connecting to the system bus: %s", pError->message);
        g_error_free (pError);
    }

    self->pPrivate->pConnection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &pError);

    if (pError)
    {
        g_error ("Panic: Failed connecting to the session bus: %s", pError->message);
        g_error_free (pError);

        return;
    }

    GSettingsSchemaSource *pSource = g_settings_schema_source_get_default ();
    GSettingsSchema *pSchema = NULL;

    if (!self->pPrivate->bGreeter)
    {
        // Get the settings
        if (pSource)
        {
            pSchema = g_settings_schema_source_lookup (pSource, "org.ayatana.indicator.a11y", FALSE);

            if (pSchema)
            {
                g_settings_schema_unref (pSchema);
                self->pPrivate->pSettings = g_settings_new ("org.ayatana.indicator.a11y");
                pSchema = g_settings_schema_source_lookup (pSource, "org.gnome.desktop.a11y.applications", FALSE);

                if (pSchema)
                {
                    g_settings_schema_unref (pSchema);
                    self->pPrivate->pOrcaSettings = g_settings_new ("org.gnome.desktop.a11y.applications");
                }
                else
                {
                    g_warning ("Panic: No org.gnome.desktop.a11y.applications schema found");
                }

                /* This is what we should use, but not all applications react to "high-contrast" setting (yet)
                pSchema = g_settings_schema_source_lookup (pSource, "org.gnome.desktop.a11y.interface", FALSE);

                if (pSchema)
                {
                    g_settings_schema_unref (pSchema);
                    self->pPrivate->pHighContrastSettings = g_settings_new ("org.gnome.desktop.a11y.interface");
                    self->pPrivate->bHighContrast = g_settings_get_boolean (self->pPrivate->pHighContrastSettings, "high-contrast");
                }
                else
                {
                    g_warning ("Panic: No org.gnome.desktop.a11y.interface schema found");
                }*/

                gboolean bMate = ayatana_common_utils_is_mate ();
                gchar *sInterface = NULL;

                if (bMate)
                {
                    sInterface = "org.mate.SettingsDaemon.plugins.media-keys";
                }
                else
                {
                    sInterface = "org.gnome.settings-daemon.plugins.media-keys";
                }

                pSchema = g_settings_schema_source_lookup (pSource, sInterface, FALSE);

                if (pSchema)
                {
                    g_settings_schema_unref (pSchema);
                    self->pPrivate->pKeybindingSettings = g_settings_new (sInterface);
                }
                else
                {
                    g_warning ("Panic: No %s schema found", sInterface);
                }

                pSchema = g_settings_schema_source_lookup (pSource, "org.gnome.desktop.a11y.applications", FALSE);

                if (pSchema)
                {
                    g_settings_schema_unref (pSchema);
                    self->pPrivate->pApplicationsSettings = g_settings_new ("org.gnome.desktop.a11y.applications");
                }
                else
                {
                    g_warning ("Panic: No org.gnome.desktop.a11y.applications schema found");
                }

                pSchema = g_settings_schema_source_lookup (pSource, "org.mate.interface", FALSE);

                if (pSchema)
                {
                    g_settings_schema_unref (pSchema);
                    self->pPrivate->pHighContrastSettings = g_settings_new ("org.mate.interface");
                    self->pPrivate->sThemeGtk = g_settings_get_string (self->pPrivate->pSettings, "gtk-theme");
                    glong nLength = g_utf8_strlen (self->pPrivate->sThemeGtk, -1);

                    if (!nLength)
                    {
                        self->pPrivate->sThemeGtk = g_settings_get_string (self->pPrivate->pHighContrastSettings, "gtk-theme");
                    }

                    self->pPrivate->sThemeIcon = g_settings_get_string (self->pPrivate->pHighContrastSettings, "icon-theme");
                    nLength = g_utf8_strlen (self->pPrivate->sThemeIcon, -1);

                    if (!nLength)
                    {
                        self->pPrivate->sThemeIcon = g_settings_get_string (self->pPrivate->pHighContrastSettings, "icon-theme");
                    }

                    self->pPrivate->sHighContrast = g_settings_get_string (self->pPrivate->pSettings, "high-contrast");
                    gboolean bThemeGtk = g_str_equal (self->pPrivate->sThemeGtk, self->pPrivate->sHighContrast);
                    gboolean bThemeIcon = g_str_equal (self->pPrivate->sThemeIcon, "ContrastHigh");
                    self->pPrivate->bHighContrast = (bThemeGtk && bThemeIcon);
                    self->pPrivate->sMagnifier = g_settings_get_string (self->pPrivate->pSettings, "magnifier");
                }
                else
                {
                    g_warning ("Panic: No org.mate.interface schema found");
                }

                pSchema = g_settings_schema_source_lookup (pSource, "org.mate.background", FALSE);

                if (pSchema)
                {
                    g_settings_schema_unref (pSchema);
                    self->pPrivate->pBackgroundSettings = g_settings_new ("org.mate.background");
                    const gchar *lProperties[] = {"color-shading-type", "picture-filename", "picture-options", "primary-color"};

                    for (guint nProperty = 0; nProperty < 4; nProperty++)
                    {
                        onBackgroundSettings (self->pPrivate->pBackgroundSettings, lProperties[nProperty], self);
                    }
                }
                else
                {
                    g_warning ("Panic: No org.mate.background schema found");
                }

                pSchema = g_settings_schema_source_lookup (pSource, "org.mate.screensaver", FALSE);

                if (pSchema)
                {
                    g_settings_schema_unref (pSchema);
                    GSettings *pSettings = g_settings_new ("org.mate.screensaver");
                    gchar *sCommand = g_settings_get_string (pSettings, "embedded-keyboard-command");
                    gboolean bSetCommand = FALSE;

                    if (!sCommand)
                    {
                        bSetCommand = TRUE;
                    }
                    else
                    {
                        glong nLength = g_utf8_strlen (sCommand, -1);
                        g_free (sCommand);

                        if (!nLength)
                        {
                            bSetCommand = TRUE;
                        }
                    }

                    if (bSetCommand)
                    {
                        g_settings_set_string (pSettings, "embedded-keyboard-command", "onboard --xid");
                    }

                    g_clear_object (&pSettings);
                }
                else
                {
                    g_warning ("Panic: No org.mate.screensaver schema found");
                }
            }
            else
            {
                g_warning ("Panic: No org.ayatana.indicator.a11y schema found");
            }
        }

        self->pPrivate->nOnboardSubscription = g_dbus_connection_signal_subscribe (self->pPrivate->pConnection, NULL, "org.freedesktop.DBus.Properties", "PropertiesChanged", "/org/onboard/Onboard/Keyboard", "org.onboard.Onboard.Keyboard", G_DBUS_SIGNAL_FLAGS_MATCH_ARG0_NAMESPACE, onOnboardBus, self, NULL);
    }
    else
    {
        // Get greeter settings
        if (pSource)
        {
            pSchema = g_settings_schema_source_lookup (pSource, GREETER_SETTINGS, FALSE);

            if (pSchema)
            {
                g_settings_schema_unref (pSchema);
                GSettings *pOnboardSettings = g_settings_new (GREETER_SETTINGS);
                self->pPrivate->bOnboardActive = g_settings_get_boolean (pOnboardSettings, "onscreen-keyboard");
                g_clear_object (&pOnboardSettings);
                self->pPrivate->pOrcaSettings = g_settings_new (GREETER_SETTINGS);
                self->pPrivate->bOrcaActive = g_settings_get_boolean (self->pPrivate->pOrcaSettings, "screen-reader");
                self->pPrivate->pHighContrastSettings = g_settings_new (GREETER_SETTINGS);
                self->pPrivate->bHighContrast = g_settings_get_boolean (self->pPrivate->pHighContrastSettings, "high-contrast");
            }
            else
            {
                g_warning ("Panic: No greeter schema found");
            }
        }

        self->pPrivate->nUserSubscription = g_dbus_connection_signal_subscribe (self->pPrivate->pConnection, NULL, GREETER_BUS_NAME, "UserChanged", GREETER_BUS_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, onUserChanged, self, NULL);
        loadManager (self);
    }

    // Create actions
    self->pPrivate->pActionGroup = g_simple_action_group_new ();

    GSimpleAction *pSimpleAction = g_simple_action_new_stateful ("_header-desktop", NULL, createHeaderState (self));
    g_action_map_add_action (G_ACTION_MAP (self->pPrivate->pActionGroup), G_ACTION (pSimpleAction));
    self->pPrivate->pHeaderAction = pSimpleAction;

    GVariant *pContrast = g_variant_new_boolean (self->pPrivate->bHighContrast);
    pSimpleAction = g_simple_action_new_stateful ("contrast", G_VARIANT_TYPE_BOOLEAN, pContrast);

    if (!self->pPrivate->bGreeter)
    {
        /* This is what we should use, but not all applications react to "high-contrast" setting (yet)
        if (self->pPrivate->pHighContrastSettings)
        {
            g_settings_bind_with_mapping (self->pPrivate->pHighContrastSettings, "high-contrast", pSimpleAction, "state", G_SETTINGS_BIND_DEFAULT, valueFromVariant, valueToVariant, NULL, NULL);
        }*/

        // Workaround for applications that do not react to "high-contrast" setting
        if (self->pPrivate->pHighContrastSettings)
        {
            g_signal_connect (self->pPrivate->pHighContrastSettings, "changed::gtk-theme", G_CALLBACK (onContrastSettings), self);
            g_signal_connect (self->pPrivate->pHighContrastSettings, "changed::icon-theme", G_CALLBACK (onContrastSettings), self);
        }

        if (self->pPrivate->pBackgroundSettings)
        {
            g_signal_connect (self->pPrivate->pBackgroundSettings, "changed::color-shading-type", G_CALLBACK (onBackgroundSettings), self);
            g_signal_connect (self->pPrivate->pBackgroundSettings, "changed::picture-filename", G_CALLBACK (onBackgroundSettings), self);
            g_signal_connect (self->pPrivate->pBackgroundSettings, "changed::picture-options", G_CALLBACK (onBackgroundSettings), self);
            g_signal_connect (self->pPrivate->pBackgroundSettings, "changed::primary-color", G_CALLBACK (onBackgroundSettings), self);
        }

        if (self->pPrivate->pSettings)
        {
            g_signal_connect (self->pPrivate->pSettings, "changed::high-contrast", G_CALLBACK (onContrastThemeSettings), self);
        }
    }

    g_action_map_add_action (G_ACTION_MAP (self->pPrivate->pActionGroup), G_ACTION (pSimpleAction));
    g_signal_connect (pSimpleAction, "change-state", G_CALLBACK (onContrastState), self);
    g_object_unref (G_OBJECT (pSimpleAction));

    GVariant *pOnboard = g_variant_new_boolean (self->pPrivate->bOnboardActive);
    pSimpleAction = g_simple_action_new_stateful ("onboard", G_VARIANT_TYPE_BOOLEAN, pOnboard);

    if (!self->pPrivate->bGreeter)
    {
        if (self->pPrivate->pApplicationsSettings)
        {
            g_settings_bind_with_mapping (self->pPrivate->pApplicationsSettings, "screen-keyboard-enabled", pSimpleAction, "state", G_SETTINGS_BIND_DEFAULT, valueFromVariant, valueToVariant, NULL, NULL);
        }
    }

    g_action_map_add_action (G_ACTION_MAP (self->pPrivate->pActionGroup), G_ACTION (pSimpleAction));
    g_signal_connect (pSimpleAction, "change-state", G_CALLBACK (onOnboardState), self);
    g_object_unref (G_OBJECT (pSimpleAction));

    GVariant *pOrca = g_variant_new_boolean (self->pPrivate->bOrcaActive);
    pSimpleAction = g_simple_action_new_stateful ("orca", G_VARIANT_TYPE_BOOLEAN, pOrca);

    if (!self->pPrivate->bGreeter)
    {
        if (self->pPrivate->pOrcaSettings)
        {
            g_settings_bind_with_mapping (self->pPrivate->pOrcaSettings, "screen-reader-enabled", pSimpleAction, "state", G_SETTINGS_BIND_DEFAULT, valueFromVariant, valueToVariant, NULL, NULL);
        }

        if (self->pPrivate->pApplicationsSettings)
        {
            g_settings_bind_with_mapping (self->pPrivate->pApplicationsSettings, "screen-reader-enabled", pSimpleAction, "state", G_SETTINGS_BIND_DEFAULT, valueFromVariant, valueToVariant, NULL, NULL);
        }
    }

    g_action_map_add_action (G_ACTION_MAP (self->pPrivate->pActionGroup), G_ACTION (pSimpleAction));
    g_signal_connect (pSimpleAction, "change-state", G_CALLBACK (onOrcaState), self);
    g_object_unref (G_OBJECT (pSimpleAction));

    GVariant *pMagnifier = g_variant_new_boolean (self->pPrivate->bMagnifierActive);
    pSimpleAction = g_simple_action_new_stateful ("magnifier", G_VARIANT_TYPE_BOOLEAN, pMagnifier);

    if (!self->pPrivate->bGreeter)
    {
        if (self->pPrivate->pApplicationsSettings)
        {
            g_settings_bind_with_mapping (self->pPrivate->pApplicationsSettings, "screen-magnifier-enabled", pSimpleAction, "state", G_SETTINGS_BIND_DEFAULT, valueFromVariant, valueToVariant, NULL, NULL);
        }
    }

    g_action_map_add_action (G_ACTION_MAP (self->pPrivate->pActionGroup), G_ACTION (pSimpleAction));
    g_signal_connect (pSimpleAction, "change-state", G_CALLBACK (onMagnifierState), self);
    g_object_unref (G_OBJECT (pSimpleAction));

    if (!self->pPrivate->bGreeter && !self->pPrivate->bScalingUnsupported)
    {
        GVariant *pScale = g_variant_new_double (1.0);
        pSimpleAction = g_simple_action_new_stateful ("scale", G_VARIANT_TYPE_DOUBLE, pScale);

        if (self->pPrivate->pSettings)
        {
            g_settings_bind_with_mapping (self->pPrivate->pSettings, "scale", pSimpleAction, "state", G_SETTINGS_BIND_DEFAULT, valueFromVariant, valueToVariant, NULL, NULL);
        }

        g_action_map_add_action (G_ACTION_MAP (self->pPrivate->pActionGroup), G_ACTION (pSimpleAction));
        g_object_unref (G_OBJECT (pSimpleAction));

        if (self->pPrivate->pSettings)
        {
            g_signal_connect_swapped (self->pPrivate->pSettings, "changed::scale", G_CALLBACK (onScaleState), self);
        }
    }

    // Add sections to the submenu
    self->pPrivate->pSubmenu = g_menu_new();
    GMenu *pSection = g_menu_new();
    GMenuItem *pItem = NULL;

    if (!self->pPrivate->bGreeter && !self->pPrivate->bScalingUnsupported)
    {
        GIcon *pIconMin = g_themed_icon_new_with_default_fallbacks ("ayatana-indicator-a11y-scale-down");
        GIcon *pIconMax = g_themed_icon_new_with_default_fallbacks ("ayatana-indicator-a11y-scale-up");
        GVariant *pIconMinSerialised = g_icon_serialize (pIconMin);
        GVariant *pIconMaxSerialised = g_icon_serialize (pIconMax);
        pItem = g_menu_item_new (_("User Interface Scale"), "indicator.scale");
        g_menu_item_set_attribute (pItem, "x-ayatana-type", "s", "org.ayatana.indicator.slider");
        g_menu_item_set_attribute_value (pItem, "min-icon", pIconMinSerialised);
        g_menu_item_set_attribute_value (pItem, "max-icon", pIconMaxSerialised);
        g_menu_item_set_attribute (pItem, "min-value", "d", 0.5);
        g_menu_item_set_attribute (pItem, "max-value", "d", 1.5);
        g_menu_item_set_attribute (pItem, "step", "d", 0.1);
        g_menu_item_set_attribute (pItem, "digits", "y", 1);
        g_menu_item_set_attribute (pItem, "marks", "b", TRUE);
        g_menu_append_item (pSection, pItem);
        g_object_unref (pIconMin);
        g_object_unref (pIconMax);
        g_variant_unref (pIconMinSerialised);
        g_variant_unref (pIconMaxSerialised);
        g_object_unref (pItem);
    }

    pItem = g_menu_item_new (_("High Contrast"), "indicator.contrast");
    g_menu_item_set_attribute (pItem, "x-ayatana-type", "s", "org.ayatana.indicator.switch");
    g_menu_append_item (pSection, pItem);
    g_object_unref (pItem);

    pItem = g_menu_item_new (_("On-Screen Keyboard"), "indicator.onboard");
    g_menu_item_set_attribute (pItem, "x-ayatana-type", "s", "org.ayatana.indicator.switch");
    setAccelerator (pItem, "on-screen-keyboard", self);
    g_menu_append_item (pSection, pItem);
    g_object_unref (pItem);

    pItem = g_menu_item_new (_("Screen Reader"), "indicator.orca");
    g_menu_item_set_attribute (pItem, "x-ayatana-type", "s", "org.ayatana.indicator.switch");
    setAccelerator (pItem, "screenreader", self);
    g_menu_append_item (pSection, pItem);
    g_object_unref (pItem);

    pItem = g_menu_item_new (_("Screen Magnifier"), "indicator.magnifier");
    g_menu_item_set_attribute (pItem, "x-ayatana-type", "s", "org.ayatana.indicator.switch");
    setAccelerator (pItem, "magnifier", self);
    g_menu_append_item (pSection, pItem);
    g_object_unref (pItem);

    g_menu_append_section (self->pPrivate->pSubmenu, NULL, G_MENU_MODEL (pSection));
    g_object_unref (pSection);

    // Add submenu to the header
    pItem = g_menu_item_new (NULL, "indicator._header-desktop");
    g_menu_item_set_attribute (pItem, "x-ayatana-type", "s", "org.ayatana.indicator.root");
    g_menu_item_set_submenu (pItem, G_MENU_MODEL (self->pPrivate->pSubmenu));
    g_object_unref (self->pPrivate->pSubmenu);

    // Add header to the menu
    self->pPrivate->pMenu = g_menu_new ();
    g_menu_append_item (self->pPrivate->pMenu, pItem);
    g_object_unref (pItem);

    self->pPrivate->nOwnId = g_bus_own_name (G_BUS_TYPE_SESSION, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT, onBusAcquired, NULL, onNameLost, self, NULL);

    if (!self->pPrivate->bGreeter && !self->pPrivate->bScalingUnsupported)
    {
        if (self->pPrivate->pSettings)
        {
            GAction *pAction = g_action_map_lookup_action (G_ACTION_MAP (self->pPrivate->pActionGroup), "scale");
            GVariant *pScale = g_settings_get_value (self->pPrivate->pSettings, "scale");
            g_action_change_state (pAction, pScale);
        }

        gint nUid = geteuid ();
        getAccountsService (self, nUid);
    }
}

static void indicator_a11y_service_class_init (IndicatorA11yServiceClass *klass)
{
    GObjectClass *pClass = G_OBJECT_CLASS(klass);
    pClass->dispose = onDispose;
    m_nSignal = g_signal_new ("name-lost", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (IndicatorA11yServiceClass, pNameLost), NULL, NULL, g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);
}

IndicatorA11yService *indicator_a11y_service_new ()
{
    GObject *pObject = g_object_new (INDICATOR_TYPE_A11Y_SERVICE, NULL);

    return INDICATOR_A11Y_SERVICE (pObject);
}
