#include "private.h"

#include <Elementary.h>
#include <stdlib.h>
#include <unistd.h>
#include "sel.h"
#include "config.h"
#include "theme.h"
#include "win.h"
#include "term_container.h"

typedef struct _Sel Sel;
typedef struct _Entry Entry;

struct _Sel
{
   Evas_Object_Smart_Clipped_Data __clipped_data;
   Evas_Object *self;
   Evas_Object *clip, *o_event;
   Ecore_Animator *anim;
   Ecore_Timer *autozoom_timeout;
   Eina_List *items;
   double t_start, t_total;
   double zoom, zoom0, zoom1;
   double last_cmd;
   double interp;
   double orig_zoom;
   Evas_Coord px, px0, px1;
   Evas_Coord py, py0, py1;
   int w, h;
   struct {
      Evas_Coord x, y;
      unsigned char down : 1;
   } down;
   Config *config;
   unsigned char select_me : 1;
   unsigned char exit_me : 1;
   unsigned char exit_on_sel : 1;
   unsigned char exit_now : 1;
   unsigned char pending_sel : 1;
   unsigned char use_px : 1;
};

struct _Entry
{
   Evas_Object *obj, *bg;
   Term_Container *tc;
   unsigned char selected : 1;
   unsigned char selected_before : 1;
   unsigned char selected_orig : 1;
   unsigned char was_selected : 1;
};

static Evas_Smart *_smart = NULL;
static Evas_Smart_Class _parent_sc = EVAS_SMART_CLASS_INIT_NULL;

static void _smart_calculate(Evas_Object *obj);
static void _transit(Sel *sd, double tim);

static void
_mouse_down_cb(void *data,
               Evas *_e EINA_UNUSED,
               Evas_Object *_obj EINA_UNUSED,
               void *event)
{
   Evas_Event_Mouse_Down *ev = event;
   Sel *sd = data;

   EINA_SAFETY_ON_NULL_RETURN(sd);

   if ((sd->exit_on_sel) || (sd->down.down) || (ev->button != 1))
     {
        return;
     }
   sd->down.x = ev->canvas.x;
   sd->down.y = ev->canvas.y;
   sd->down.down = EINA_TRUE;
}

static void
_mouse_up_cb(void *data,
             Evas *_e EINA_UNUSED,
             Evas_Object *_obj EINA_UNUSED,
             void *event)
{
   Evas_Event_Mouse_Up *ev = event;
   Sel *sd = data;
   Evas_Coord dx, dy;
   Evas_Coord finger_size;

   EINA_SAFETY_ON_NULL_RETURN(sd);

   if ((sd->exit_on_sel) || (!sd->down.down))
     {
        return;
     }
   sd->down.down = EINA_FALSE;
   dx = abs(ev->canvas.x - sd->down.x);
   dy = abs(ev->canvas.y - sd->down.y);
   finger_size = elm_config_finger_size_get();
   if ((dx <= finger_size) &&
       (dy <= finger_size))
     {
        Eina_List *l;
        Entry *en;
        Evas_Coord x, y, ox, oy, ow, oh;

        x = ev->canvas.x;
        y = ev->canvas.y;
        EINA_LIST_FOREACH(sd->items, l, en)
          {
             evas_object_geometry_get(en->bg, &ox, &oy, &ow, &oh);
             if (ELM_RECTS_INTERSECT(ox, oy, ow, oh, x, y, 1, 1))
               {
                  sel_entry_selected_set(sd->self, en->obj, EINA_FALSE);
                  sd->select_me = EINA_TRUE;
                  sd->exit_me = EINA_FALSE;
                  if (sd->autozoom_timeout)
                    {
                       ecore_timer_del(sd->autozoom_timeout);
                       sd->autozoom_timeout = NULL;
                    }
                  evas_object_smart_callback_call(sd->self, "ending", NULL);
                  sel_zoom(sd->self, 1.0);
                  return;
               }
          }
        evas_object_smart_callback_call(sd->self, "clicked", NULL);
     }
}

static void
_mouse_move_cb(void *data,
               Evas *_e EINA_UNUSED,
               Evas_Object *_obj EINA_UNUSED,
               void *event)
{
   Evas_Event_Mouse_Move *ev = event;
   Sel *sd = data;

   EINA_SAFETY_ON_NULL_RETURN(sd);

   Evas_Coord x = 0, y = 0, w = 0, h = 0, sw, sh;
   int iw, ih;

   if ((sd->exit_me) || (sd->exit_now) || (sd->select_me) || (sd->exit_on_sel))
     return;
   iw = sqrt(eina_list_count(sd->items));
   if (iw < 1) iw = 1;
   ih = (eina_list_count(sd->items) + (iw - 1)) / iw;
   if (ih < 1) ih = 1;
   evas_object_geometry_get(sd->self, &x, &y, &w, &h);
   sw = w * sd->zoom;
   sh = h * sd->zoom;
   if (!sd->down.down)
     {
        if ((w > 0) && (h > 0))
          {
             _transit(sd, 0.5);
             sd->px1 = ((ev->cur.canvas.x - x) * ((iw - 1) * sw)) / w;
             sd->py1 = ((ev->cur.canvas.y - y) * ((ih - 1) * sh)) / h;
             sd->use_px = EINA_TRUE;
          }
     }
   else
     {
        // XXX: drag to scroll
     }
}

static Eina_Bool
_autozoom_reset(void *data)
{
   Sel *sd = data;
   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, EINA_FALSE);

   sd->autozoom_timeout = NULL;
   sel_zoom(data, sd->orig_zoom);

   return EINA_FALSE;
}

static void
_autozoom(Sel *sd)
{
   double t = ecore_loop_time_get();

   EINA_SAFETY_ON_NULL_RETURN(sd);

   if ((t - sd->last_cmd) < 0.5)
     {
        sel_zoom(sd->self, sd->zoom * 0.9);
     }
   sd->last_cmd = t;
   if (sd->autozoom_timeout)
     {
        ecore_timer_del(sd->autozoom_timeout);
     }
   sd->autozoom_timeout = ecore_timer_add(0.5, _autozoom_reset, sd);
}


void
sel_key_down(Evas_Object *obj,
             Evas_Event_Key_Down *event)
{
   Evas_Event_Key_Down *ev = event;
   Sel *sd = evas_object_smart_data_get(obj);
   Eina_List *l;
   Entry *en;

   EINA_SAFETY_ON_NULL_RETURN(sd);

   if ((!strcmp(ev->key, "Next")) ||
       (!strcmp(ev->key, "Right")))
     {
        EINA_LIST_FOREACH(sd->items, l, en)
          {
             if (en->selected)
               {
                  if (l->next)
                    {
                       en = l->next->data;
                       sel_entry_selected_set(obj, en->obj, EINA_FALSE);
                       break;
                    }
                  else
                    {
                       return;
                    }
               }
          }
        sd->exit_now = EINA_FALSE;
        _autozoom(sd);
     }
   else if ((!strcmp(ev->key, "Prior")) ||
            (!strcmp(ev->key, "Left")))
     {
        EINA_LIST_FOREACH(sd->items, l, en)
          {
             if (en->selected)
               {
                  if (l->prev)
                    {
                       en = l->prev->data;
                       sel_entry_selected_set(obj, en->obj, EINA_FALSE);
                       break;
                    }
                  else
                    {
                       return;
                    }
               }
          }
        sd->exit_now = EINA_FALSE;
        _autozoom(sd);
     }
   else if (!strcmp(ev->key, "Up"))
     {
        EINA_LIST_FOREACH(sd->items, l, en)
          {
             if (en->selected)
               {
                  Evas_Coord x = 0, y = 0, w = 0, h = 0, sgx, sgy;
                  Eina_Bool found = EINA_FALSE;

                  evas_object_geometry_get(en->bg, &x, &y, &w, &h);
                  sgx = x + (w / 2);
                  sgy = y - (h / 2);
                  EINA_LIST_FOREACH(sd->items, l, en)
                    {
                       evas_object_geometry_get(en->bg, &x, &y, &w, &h);
                       if (ELM_RECTS_INTERSECT(x, y, w, h, sgx, sgy, 1, 1))
                         {
                            sel_entry_selected_set(obj, en->obj, EINA_FALSE);
                            found = EINA_TRUE;
                            break;
                         }
                    }
                  if (found == EINA_FALSE)
                    {
                       return;
                    }
                  break;
               }
          }
        sd->exit_now = EINA_FALSE;
        _autozoom(sd);
     }
   else if (!strcmp(ev->key, "Down"))
     {
        EINA_LIST_FOREACH(sd->items, l, en)
          {
             if (en->selected)
               {
                  Evas_Coord x = 0, y = 0, w = 0, h = 0, sgx, sgy;
                  Eina_Bool found = EINA_FALSE;

                  evas_object_geometry_get(en->bg, &x, &y, &w, &h);
                  sgx = x + (w / 2);
                  sgy = y + h + (h / 2);
                  EINA_LIST_FOREACH(sd->items, l, en)
                    {
                       evas_object_geometry_get(en->bg, &x, &y, &w, &h);
                       if (ELM_RECTS_INTERSECT(x, y, w, h, sgx, sgy, 1, 1))
                         {
                            sel_entry_selected_set(obj, en->obj, EINA_FALSE);
                            found = EINA_TRUE;
                            break;
                         }
                    }
                  if (found == EINA_FALSE)
                    {
                       return;
                    }
                  break;
               }
          }
        sd->exit_now = EINA_FALSE;
        _autozoom(sd);
     }
   else if ((!strcmp(ev->key, "Return")) ||
            (!strcmp(ev->key, "KP_Enter")) ||
            (!strcmp(ev->key, "space")))
     {
        sd->select_me = EINA_TRUE;
        sd->exit_me = EINA_FALSE;
        if (sd->autozoom_timeout)
          {
             ecore_timer_del(sd->autozoom_timeout);
             sd->autozoom_timeout = NULL;
          }
        evas_object_smart_callback_call(obj, "ending", NULL);
        sel_zoom(obj, 1.0);
     }
   else if (!strcmp(ev->key, "Escape"))
     {
        Evas_Object *entry = NULL;

        EINA_LIST_FOREACH(sd->items, l, en)
          {
             if (en->selected_orig) entry = en->obj;
          }
        if (entry) sel_entry_selected_set(obj, entry, EINA_FALSE);
        sd->select_me = EINA_FALSE;
        sd->exit_me = EINA_TRUE;
        if (sd->autozoom_timeout)
          {
             ecore_timer_del(sd->autozoom_timeout);
             sd->autozoom_timeout = NULL;
          }
        evas_object_smart_callback_call(obj, "ending", NULL);
        sel_zoom(obj, 1.0);
     }
}

static void
_layout(Sel *sd)
{
   int iw, ih, x, y;
   Evas_Coord ox, oy, ow, oh, w, h, px, py, ww, hh;
   Eina_List *l;
   Entry *en;

   EINA_SAFETY_ON_NULL_RETURN(sd);

   iw = sqrt(eina_list_count(sd->items));
   if (iw < 1) iw = 1;
   ih = (eina_list_count(sd->items) + (iw - 1)) / iw;
   if (ih < 1) ih = 1;
   evas_object_geometry_get(sd->self, &ox, &oy, &ow, &oh);
   w = ow * sd->zoom;
   h = oh * sd->zoom;
   x = y = 0;
   if (!sd->use_px)
     {
        EINA_LIST_FOREACH(sd->items, l, en)
          {
             if (en->selected_before)
               {
                  sd->px0 = (x * w);
                  sd->py0 = (y * h);
               }
             if ((sd->exit_on_sel) && (!sd->exit_now))
               {
                  if (en->selected_before)
                    {
                       sd->px1 = (x * w);
                       sd->py1 = (y * h);
                    }
               }
             else
               {
                  if (en->selected)
                    {
                       sd->px1 = (x * w);
                       sd->py1 = (y * h);
                    }
               }
             x++;
             if (x >= iw)
               {
                  x = 0;
                  y++;
               }
          }
     }

   sd->px = sd->px0 + ((sd->px1 - sd->px0) * sd->interp);
   sd->py = sd->py0 + ((sd->py1 - sd->py0) * sd->interp);

   px = sd->px;
   py = sd->py;

   if ((iw > 0) && (w > 0) && (ih > 0) && (h > 0))
     {
        ww = iw * w;
        hh = ih * h;
        if (ww <= w) px = (ww - ow) / 2;
        else px = px - ((px * (ow - w)) / (ww - w));
        if (hh <= h) py = (hh - oh) / 2;
        else py = py - ((py * (oh - h)) / (hh - h));
     }
   x = y = 0;

   EINA_LIST_FOREACH(sd->items, l, en)
     {
        evas_object_move(en->bg, ox + (x * w) - px, oy + (y * h) - py);
        evas_object_resize(en->bg, w, h);
        evas_object_show(en->bg);
        x++;
        if (x >= iw)
          {
             x = 0;
             y++;
          }
     }
   if ((sd->w > 0) && (sd->h > 0) && (sd->pending_sel))
     {
        sd->pending_sel = EINA_FALSE;
        EINA_LIST_FOREACH(sd->items, l, en)
          {
             if ((!en->was_selected) && (en->selected))
               edje_object_signal_emit(en->bg, "selected", "terminology");
             else if ((en->was_selected) && (!en->selected))
               edje_object_signal_emit(en->bg, "unselected", "terminology");
          }
     }
}

static Eina_Bool
_anim_cb(void *data)
{
   Sel *sd = data;
   double t = 1.0;

   EINA_SAFETY_ON_NULL_RETURN_VAL(sd, EINA_FALSE);

   if (sd->t_total > 0.0)
     {
        t = (ecore_loop_time_get() - sd->t_start) / sd->t_total;
        if (t < 0.0) t = 0.0;
        else if (t > 1.0) t = 1.0;
     }
   sd->interp = ecore_animator_pos_map(t, ECORE_POS_MAP_DECELERATE, 0, 0);
   sd->zoom = sd->zoom0 + ((sd->zoom1 - sd->zoom0) * sd->interp);
   _layout(sd);
   if (t >= 1.0)
     {
        sd->anim = NULL;
        if ((sd->exit_on_sel) && (!sd->exit_now))
          {
             if (sd->autozoom_timeout)
               {
                  ecore_timer_del(sd->autozoom_timeout);
                  sd->autozoom_timeout = NULL;
               }
             sd->exit_now = EINA_TRUE;
             evas_object_smart_callback_call(sd->self, "ending", NULL);
             sel_zoom(sd->self, 1.0);
             return EINA_FALSE;
          }
        if ((sd->select_me) || (sd->exit_now))
          {
             Eina_List *l;
             Entry *en;
             Evas_Object *entry = NULL;

             EINA_LIST_FOREACH(sd->items, l, en)
               {
                  if (en->selected)
                    {
                       entry = en->obj;
                    }
               }
             if (entry)
               {
                  evas_object_smart_callback_call(sd->self, "selected", entry);
               }
          }
        else if (sd->exit_me)
          {
             evas_object_smart_callback_call(sd->self, "exit", NULL);
          }
        return EINA_FALSE;
     }
   return EINA_TRUE;
}

static void
_transit(Sel *sd, double tim)
{
   EINA_SAFETY_ON_NULL_RETURN(sd);

   sd->px0 = sd->px;
   sd->py0 = sd->py;
   sd->zoom0 = sd->zoom;
   sd->t_start = ecore_loop_time_get();
   sd->t_total = tim;
   sd->interp = 0.0;
   if (!sd->anim)
     {
        sd->anim = ecore_animator_add(_anim_cb, sd);
     }
}

static void
_label_redo(Entry *en)
{
   const char *s;

   if (!en->obj || !en->tc)
     return;
   s = en->tc->title;
   if (!s)
     s = "Terminology";
   edje_object_part_text_set(en->bg, "terminology.label", s);
}

void
sel_entry_title_set(void *entry, const char *title)
{
   Entry *en = entry;

   edje_object_part_text_set(en->bg, "terminology.label", title);
}

void
sel_entry_close(void *data)
{
   Entry *en = data;

   en->tc = NULL;
}

void
sel_entry_update(void *data)
{
   Entry *en = data;

   en->tc = evas_object_data_get(en->obj, "tc");
   if (en->tc)
     {
        _label_redo(en);
     }
}

static void
_entry_del_cb(void *data,
              Evas *_e EINA_UNUSED,
              Evas_Object *_obj EINA_UNUSED,
              void *_info EINA_UNUSED)
{
   Entry *en = data;
   if (en->obj)
     evas_object_event_callback_del_full(en->obj, EVAS_CALLBACK_DEL,
                                         _entry_del_cb, en);
   en->obj = NULL;
   /*
   if (en->termio)
     {
        evas_object_smart_callback_del_full(en->termio, "title,change",
                                            _title_cb, en);
        evas_object_smart_callback_del_full(en->termio, "icon,change",
                                            _icon_cb, en);
        evas_object_smart_callback_del_full(en->termio, "bell",
                                            _bell_cb, en);
        en->termio = NULL;
     }
   */
}

static void
_smart_add(Evas_Object *obj)
{
   Sel *sd;
   Evas_Object *o;

   sd = calloc(1, sizeof(Sel));
   EINA_SAFETY_ON_NULL_RETURN(sd);
   evas_object_smart_data_set(obj, sd);

   _parent_sc.add(obj);

   o = evas_object_rectangle_add(evas_object_evas_get(obj));
   evas_object_smart_member_add(o, obj);
   sd->clip = o;
   evas_object_color_set(o, 255, 255, 255, 255);
}

static void
_smart_del(Evas_Object *obj)
{
   Sel *sd = evas_object_smart_data_get(obj);
   Entry *en;
   if (!sd) return;
   if (sd->clip) evas_object_del(sd->clip);
   if (sd->o_event) evas_object_del(sd->o_event);
   if (sd->anim) ecore_animator_del(sd->anim);
   if (sd->autozoom_timeout) ecore_timer_del(sd->autozoom_timeout);
   EINA_LIST_FREE(sd->items, en)
     {
        if (en->obj)
          evas_object_event_callback_del_full(en->obj, EVAS_CALLBACK_DEL,
                                              _entry_del_cb, en);
        if (en->obj) evas_object_del(en->obj);
        evas_object_del(en->bg);
        free(en);
     }
   _parent_sc.del(obj);
   free(sd);
}

static void
_smart_resize(Evas_Object *obj, Evas_Coord w, Evas_Coord h)
{
   Sel *sd = evas_object_smart_data_get(obj);
   Evas_Coord ow, oh;
   if (!sd) return;
   evas_object_geometry_get(obj, NULL, NULL, &ow, &oh);
   if ((ow == w) && (oh == h)) return;
   evas_object_smart_changed(obj);
}

static void
_smart_calculate(Evas_Object *obj)
{
   Sel *sd = evas_object_smart_data_get(obj);
   Evas_Coord ox, oy, ow, oh;

   if (!sd) return;
   evas_object_geometry_get(obj, &ox, &oy, &ow, &oh);
   sd->w = ow;
   sd->h = oh;
   evas_object_move(sd->clip, ox, oy);
   evas_object_resize(sd->clip, ow, oh);
   evas_object_move(sd->o_event, ox, oy);
   evas_object_resize(sd->o_event, ow, oh);
   _layout(sd);
}

static void
_smart_move(Evas_Object *obj,
            Evas_Coord _x EINA_UNUSED,
            Evas_Coord _y EINA_UNUSED)
{
   Sel *sd = evas_object_smart_data_get(obj);
   if (!sd) return;
   evas_object_smart_changed(obj);
}
static void
_smart_init(void)
{
   static Evas_Smart_Class sc;

   evas_object_smart_clipped_smart_set(&_parent_sc);
   sc           = _parent_sc;
   sc.name      = "sel";
   sc.version   = EVAS_SMART_CLASS_VERSION;
   sc.add       = _smart_add;
   sc.del       = _smart_del;
   sc.resize    = _smart_resize;
   sc.move      = _smart_move;
   sc.calculate = _smart_calculate;
   _smart = evas_smart_class_new(&sc);
}

Evas_Object *
sel_add(Evas_Object *parent)
{
   Evas *e;
   Evas_Object *obj;
   Sel *sd;

   EINA_SAFETY_ON_NULL_RETURN_VAL(parent, NULL);
   e = evas_object_evas_get(parent);
   if (!e) return NULL;

   if (!_smart) _smart_init();
   obj = evas_object_smart_add(e, _smart);
   sd = evas_object_smart_data_get(obj);
   sd->self = obj;

   sd->o_event = evas_object_rectangle_add(e);
   evas_object_color_set(sd->o_event, 0, 0, 0, 0);
   evas_object_repeat_events_set(sd->o_event, EINA_TRUE);
   evas_object_smart_member_add(sd->o_event, obj);
   evas_object_clip_set(sd->o_event, sd->clip);
   evas_object_show(sd->o_event);
   evas_object_event_callback_add(sd->o_event, EVAS_CALLBACK_MOUSE_DOWN,
                                  _mouse_down_cb, sd);
   evas_object_event_callback_add(sd->o_event, EVAS_CALLBACK_MOUSE_UP,
                                  _mouse_up_cb, sd);
   evas_object_event_callback_add(sd->o_event, EVAS_CALLBACK_MOUSE_MOVE,
                                  _mouse_move_cb, sd);
   sd->zoom = 1.0;

   return obj;
}

void *
sel_entry_add(Evas_Object *obj, Evas_Object *entry,
              Eina_Bool selected, Eina_Bool bell, Config *config)
{
   Sel *sd = evas_object_smart_data_get(obj);
   Entry *en = calloc(1, sizeof(Entry));
   if (!en) return NULL;
   sd->items = eina_list_append(sd->items, en);
   sd->config = config;
   en->obj = entry;
   en->selected = selected;
   en->selected_before = selected;
   en->selected_orig = selected;
   en->bg = edje_object_add(evas_object_evas_get(obj));
   theme_apply(en->bg, config, "terminology/sel/item",
               NULL, NULL, EINA_FALSE);
   evas_object_smart_member_add(en->bg, obj);
   evas_object_clip_set(en->bg, sd->clip);
   edje_object_part_swallow(en->bg, "terminology.content", en->obj);
   evas_object_show(en->obj);

   evas_object_stack_below(en->bg, sd->o_event);
   if (en->selected)
     {
        edje_object_signal_emit(en->bg, "selected,start", "terminology");
        edje_object_message_signal_process(en->bg);
     }
   if (bell)
     {
        edje_object_signal_emit(en->bg, "bell", "terminology");
        if (!config->bell_rings)
          edje_object_signal_emit(en->bg, "bell,ring", "terminology");

        edje_object_message_signal_process(en->bg);
     }
   sd->interp = 1.0;
   en->tc = evas_object_data_get(en->obj, "tc");
   if (en->tc)
     {
        _label_redo(en);
     }
   evas_object_event_callback_add(en->obj, EVAS_CALLBACK_DEL,
                                  _entry_del_cb, en);

   return en;
}

void
sel_go(Evas_Object *obj)
{
   Sel *sd = evas_object_smart_data_get(obj);
   Eina_List *l;
   Entry *en;

   EINA_SAFETY_ON_NULL_RETURN(sd);

   _layout(sd);
   evas_object_show(sd->clip);
   EINA_LIST_FOREACH(sd->items, l, en)
     {
        if (en->selected)
          {
             evas_object_stack_below(en->bg, sd->o_event);
             break;
          }
     }
}

void
sel_entry_selected_set(Evas_Object *obj, Evas_Object *entry, Eina_Bool keep_before)
{
   Sel *sd = evas_object_smart_data_get(obj);
   Eina_List *l;
   Entry *en;
   Config *config;
   if (!sd) return;

   config = sd->config;
   if (!config) return;

   /* Unselect previous selected item */
   EINA_LIST_FOREACH(sd->items, l, en)
     {
        if ( (en->obj != entry) && (en->selected) )
          {
             if ((sd->w > 0) && (sd->h > 0))
               edje_object_signal_emit(en->bg, "unselected", "terminology");
             else
               {
                  en->was_selected = EINA_TRUE;
                  sd->pending_sel = EINA_TRUE;
               }
             en->selected = EINA_FALSE;
             sd->select_me = EINA_FALSE;
             sd->exit_me = EINA_FALSE;
          }
        if (!keep_before) en->selected_before = EINA_FALSE;
     }
   /* Select new item */
   EINA_LIST_FOREACH(sd->items, l, en)
     {
        if (en->obj == entry)
          {
             en->was_selected = EINA_FALSE;
             en->selected = EINA_TRUE;
             if ((sd->w > 0) && (sd->h > 0))
               edje_object_signal_emit(en->bg, "selected", "terminology");
             else
               sd->pending_sel = EINA_TRUE;
             evas_object_stack_below(en->bg, sd->o_event);
          }
     }
   sd->use_px = EINA_FALSE;
   _transit(sd, config->tab_zoom);
}

void
sel_zoom(Evas_Object *obj, double zoom)
{
   Sel *sd = evas_object_smart_data_get(obj);
   if (!sd) return;
   Config *config = sd->config;
   if (!config) return;

   sd->zoom1 = zoom;
   _transit(sd, config->tab_zoom);
}

void
sel_orig_zoom_set(Evas_Object *obj, double zoom)
{
   Sel *sd = evas_object_smart_data_get(obj);
   if (!sd) return;
   sd->orig_zoom = zoom;
}

void
sel_exit(Evas_Object *obj)
{
   Sel *sd = evas_object_smart_data_get(obj);
   if (!sd) return;
   sd->exit_on_sel = EINA_TRUE;
}
