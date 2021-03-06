/*
 * Copyright © 2013-2015  Rinat Ibragimov
 *
 * This file is part of FreshPlayerPlugin.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "ppb_flash_menu.h"
#include "ppb_core.h"
#include "ppb_message_loop.h"
#include <pthread.h>
#include <stdlib.h>
#include "trace.h"
#include "tables.h"
#include "pp_resource.h"
#include <ppapi/c/pp_errors.h>
#include <gtk/gtk.h>
#include "pp_interface.h"


static int32_t                     *popup_menu_result = NULL;
static struct PP_CompletionCallback popup_menu_ccb = { };
static int                          popup_menu_sentinel = 0;
static int                          popup_menu_canceled = 0;


// called when used selects menu item
static
void
menu_item_activated(GtkMenuItem *mi, gpointer user_data)
{
    if (popup_menu_result)
        *popup_menu_result = (size_t)user_data;

    // set the flag indicating user selected something, not just aborted
    popup_menu_canceled = 0;
}

// called when used selects menu item (workaround for submenus)
static
void
menu_item_button_press(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    if (popup_menu_result)
        *popup_menu_result = (size_t)user_data;

    // set the flag indicating user selected something, not just aborted
    popup_menu_canceled = 0;
}

// called when menu is closed
static
void
menu_selection_done(GtkMenuShell *object, gboolean user_data)
{
    int32_t code = popup_menu_canceled ? PP_ERROR_USERCANCEL : PP_OK;

    ppb_core_call_on_main_thread2(0, popup_menu_ccb, code, __func__);

    popup_menu_sentinel = 0;
    popup_menu_result = NULL;
}

static
GtkWidget *
convert_menu(const struct PP_Flash_Menu *pp_menu)
{
    GtkWidget *menu = gtk_menu_new();

    for (uintptr_t k = 0; k < pp_menu->count; k ++) {
        const struct PP_Flash_MenuItem pp_mi = pp_menu->items[k];
        GtkWidget *mi = NULL;

        switch (pp_mi.type) {
        case PP_FLASH_MENUITEM_TYPE_NORMAL:
            mi = gtk_menu_item_new_with_label(pp_mi.name);
            break;
        case PP_FLASH_MENUITEM_TYPE_CHECKBOX:
            mi = gtk_check_menu_item_new_with_label(pp_mi.name);
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), pp_mi.checked != PP_FALSE);
            break;
        case PP_FLASH_MENUITEM_TYPE_SEPARATOR:
            mi = gtk_separator_menu_item_new();
            break;
        case PP_FLASH_MENUITEM_TYPE_SUBMENU:
            mi = gtk_menu_item_new_with_label(pp_mi.name);
            break;
        }

        if (!mi)
            continue;

        gtk_widget_set_sensitive(mi, pp_mi.enabled != PP_FALSE);
        gtk_widget_show(mi);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

        if (pp_mi.type == PP_FLASH_MENUITEM_TYPE_SUBMENU) {
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), convert_menu(pp_mi.submenu));
        } else {
            // each menu item have specific id associated
            g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menu_item_activated),
                             (void*)(size_t)pp_mi.id);
            // connect "button-press-event" to workaround submenu "activate" signal missing issue
            g_signal_connect(G_OBJECT(mi), "button-press-event", G_CALLBACK(menu_item_button_press),
                             (void*)(size_t)pp_mi.id);
        }
    }

    return menu;
}

struct flash_menu_create_param_s {
    PP_Resource                 flash_menu;
    const struct PP_Flash_Menu *menu_data;
    PP_Resource                 m_loop;
    int                         depth;
};

static
void
flash_menu_create_ptac(void *param)
{
    struct flash_menu_create_param_s *p = param;
    struct pp_flash_menu_s *fm = pp_resource_acquire(p->flash_menu, PP_RESOURCE_FLASH_MENU);
    if (!fm) {
        trace_error("%s, bad resource\n", __func__);
        goto quit;
    }

    // recursively construct menu
    fm->menu = convert_menu(p->menu_data);

    // we need notification on menu close
    g_signal_connect(fm->menu, "selection-done", G_CALLBACK(menu_selection_done), NULL);

    pp_resource_release(p->flash_menu);
quit:
    ppb_message_loop_post_quit_depth(p->m_loop, PP_FALSE, p->depth);
}

static
void
flash_menu_create_comt(void *user_data, int32_t result)
{
    ppb_core_call_on_browser_thread(0, flash_menu_create_ptac, user_data);
}

PP_Resource
ppb_flash_menu_create(PP_Instance instance_id, const struct PP_Flash_Menu *menu_data)
{
    struct pp_instance_s *pp_i = tables_get_pp_instance(instance_id);
    if (!pp_i) {
        trace_error("%s, bad instance\n", __func__);
        return 0;
    }

    PP_Resource flash_menu = pp_resource_allocate(PP_RESOURCE_FLASH_MENU, pp_i);
    if (pp_resource_get_type(flash_menu) != PP_RESOURCE_FLASH_MENU) {
        trace_error("%s, resource allocation failure\n", __func__);
        return 0;
    }

    struct flash_menu_create_param_s *p = g_slice_alloc0(sizeof(*p));

    p->flash_menu = flash_menu;
    p->menu_data =  menu_data;
    p->m_loop =     ppb_message_loop_get_current();
    p->depth =      ppb_message_loop_get_depth(p->m_loop) + 1;

    ppb_message_loop_post_work_with_result(p->m_loop, PP_MakeCCB(flash_menu_create_comt, p), 0,
                                           PP_OK, p->depth, __func__);
    ppb_message_loop_run_nested(p->m_loop);

    g_slice_free1(sizeof(*p), p);
    return flash_menu;
}

static
void
destroy_flash_menu_ptac(void *param)
{
    GtkWidget *menu = param;
    g_object_unref(menu);
}

static
void
ppb_flash_menu_destroy(void *p)
{
    struct pp_flash_menu_s *fm = p;

    g_object_ref_sink(fm->menu);

    // actual menu destroy can make something X-related, call in on browser thread
    ppb_core_call_on_browser_thread(fm->instance->id, destroy_flash_menu_ptac, fm->menu);
}

PP_Bool
ppb_flash_menu_is_flash_menu(PP_Resource resource_id)
{
    return pp_resource_get_type(resource_id) == PP_RESOURCE_FLASH_MENU;
}

static
void
menu_popup_ptac(void *p)
{
    GtkWidget *menu = p;
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, 3, gtk_get_current_event_time());
}

int32_t
ppb_flash_menu_show(PP_Resource menu_id, const struct PP_Point *location, int32_t *selected_id,
                    struct PP_CompletionCallback callback)
{
    struct pp_flash_menu_s *fm = pp_resource_acquire(menu_id, PP_RESOURCE_FLASH_MENU);
    if (!fm) {
        trace_error("%s, bad resource\n", __func__);
        return PP_ERROR_BADRESOURCE;
    }
    struct pp_instance_s *pp_i = fm->instance;

    if (popup_menu_sentinel)
        trace_error("%s, two context menus at the same time\n", __func__);

    (void)location; // TODO: handle location

    popup_menu_sentinel = 1;
    popup_menu_canceled = 1;
    popup_menu_ccb = callback;
    popup_menu_result = selected_id;

    pthread_mutex_lock(&display.lock);
    // creating and showing menu together with its closing generates pair of focus events,
    // FocusOut and FocusIn, which should not be passed to the plugin instance. Otherwise they
    // will tamper with text selection.
    pp_i->ignore_focus_events_cnt = 2;
    pthread_mutex_unlock(&display.lock);

    ppb_core_call_on_browser_thread(pp_i->id, menu_popup_ptac, fm->menu);

    pp_resource_release(menu_id);
    return PP_OK_COMPLETIONPENDING;
}


// trace wrappers
TRACE_WRAPPER
PP_Resource
trace_ppb_flash_menu_create(PP_Instance instance_id, const struct PP_Flash_Menu *menu_data)
{
    trace_info("[PPB] {full} %s instance_id=%d, menu_data=%p\n", __func__+6, instance_id,
               menu_data);
    return ppb_flash_menu_create(instance_id, menu_data);
}

TRACE_WRAPPER
PP_Bool
trace_ppb_flash_menu_is_flash_menu(PP_Resource resource_id)
{
    trace_info("[PPB] {full} %s resource_id=%d\n", __func__+6, resource_id);
    return ppb_flash_menu_is_flash_menu(resource_id);
}

TRACE_WRAPPER
int32_t
trace_ppb_flash_menu_show(PP_Resource menu_id, const struct PP_Point *location,
                          int32_t *selected_id, struct PP_CompletionCallback callback)
{
    gchar *s_location = trace_point_as_string(location);
    trace_info("[PPB] {full} %s menu_id=%d, location=%s, callback={.func=%p, .user_data=%p, "
               ".flags=%d}\n", __func__+6, menu_id, s_location, callback.func, callback.user_data,
               callback.flags);
    g_free(s_location);
    return ppb_flash_menu_show(menu_id, location, selected_id, callback);
}


const struct PPB_Flash_Menu_0_2 ppb_flash_menu_interface_0_2 = {
    .Create =       TWRAPF(ppb_flash_menu_create),
    .IsFlashMenu =  TWRAPF(ppb_flash_menu_is_flash_menu),
    .Show =         TWRAPF(ppb_flash_menu_show),
};

static
void
__attribute__((constructor))
constructor_ppb_flash_menu(void)
{
    register_interface(PPB_FLASH_MENU_INTERFACE_0_2, &ppb_flash_menu_interface_0_2);
    register_resource(PP_RESOURCE_FLASH_MENU, ppb_flash_menu_destroy);
}
