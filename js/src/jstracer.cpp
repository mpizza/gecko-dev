/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99:
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla SpiderMonkey JavaScript 1.9 code, released
 * May 28, 2008.
 *
 * The Initial Developer of the Original Code is
 *   Brendan Eich <brendan@mozilla.org>
 *
 * Contributor(s):
 *   Andreas Gal <gal@mozilla.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include <math.h>

#include "nanojit/avmplus.h"
#include "nanojit/nanojit.h"
#include "jsarray.h"
#include "jsbool.h"
#include "jscntxt.h"
#include "jsobj.h"
#include "jsfun.h"
#include "jsinterp.h"
#include "jsprf.h"
#include "jsscript.h"
#include "jsscope.h"
#include "jstracer.h"

#include "jsautooplen.h"

#ifdef _MSC_VER
#include <malloc.h>
#define alloca _alloca
#endif

#ifdef DEBUG
#define ABORT_TRACE(msg)   do { fprintf(stderr, "abort: %d: %s\n", __LINE__, msg); return false; } while(0)
#else
#define ABORT_TRACE(msg)   return false
#endif

using namespace avmplus;
using namespace nanojit;

static GC gc = GC();
static avmplus::AvmCore* core = new (&gc) avmplus::AvmCore();

Tracker::Tracker()
{
    pagelist = 0;
}

Tracker::~Tracker()
{
    clear();
}

jsuword
Tracker::getPageBase(const void* v) const
{
    return jsuword(v) & ~jsuword(NJ_PAGE_SIZE-1);
}

struct Tracker::Page*
Tracker::findPage(const void* v) const
{
    jsuword base = getPageBase(v);
    struct Tracker::Page* p = pagelist;
    while (p) {
        if (p->base == base) {
            return p;
        }
        p = p->next;
    }
    return 0;
}

struct Tracker::Page*
Tracker::addPage(const void* v) {
    jsuword base = getPageBase(v);
    struct Tracker::Page* p = (struct Tracker::Page*)
        GC::Alloc(sizeof(*p) - sizeof(p->map) + (NJ_PAGE_SIZE >> 2) * sizeof(LIns*));
    p->base = base;
    p->next = pagelist;
    pagelist = p;
    return p;
}

void
Tracker::clear()
{
    while (pagelist) {
        Page* p = pagelist;
        pagelist = pagelist->next;
        GC::Free(p);
    }
}

LIns* 
Tracker::get(const void* v) const
{
    struct Tracker::Page* p = findPage(v);
    JS_ASSERT(p != 0); /* we must have a page for the slot we are looking for */
    LIns* i = p->map[(jsuword(v) & 0xfff) >> 2];
    JS_ASSERT(i != 0);
    return i;
}

void
Tracker::set(const void* v, LIns* i)
{
    struct Tracker::Page* p = findPage(v);
    if (!p)
        p = addPage(v);
    p->map[(jsuword(v) & 0xfff) >> 2] = i;
}

/*
 * Return the coerced type of a value. If it's a number, we always return JSVAL_DOUBLE, no matter
 * whether it's represented as an int or as a double.
 */
static inline int getCoercedType(jsval v)
{
    if (JSVAL_IS_INT(v))
        return JSVAL_DOUBLE;
    return JSVAL_TAG(v);
}

static inline bool isNumber(jsval v)
{
    return JSVAL_IS_INT(v) || JSVAL_IS_DOUBLE(v);
}

static inline jsdouble asNumber(jsval v)
{
    JS_ASSERT(isNumber(v));
    if (JSVAL_IS_DOUBLE(v))
        return *JSVAL_TO_DOUBLE(v);
    return (jsdouble)JSVAL_TO_INT(v);
}

static inline bool isInt32(jsval v)
{
    if (!isNumber(v))
        return false;
    jsdouble d = asNumber(v);
    return d == (jsint)d;
}

static LIns* demote(LirWriter *out, LInsp i)
{
    if (i->isCall())
        return callArgN(i,0);
    if (i->isop(LIR_i2f) || i->isop(LIR_u2f))
        return i->oprnd1();
    AvmAssert(i->isconstq());
    double cf = i->constvalf();
    int32_t ci = cf > 0x7fffffff ? uint32_t(cf) : int32_t(cf);
    return out->insImm(ci);
}

static bool isPromoteInt(LIns *i)
{
    jsdouble d;
    return i->isop(LIR_i2f) || (i->isconstq() && ((d = i->constvalf()) == (jsdouble)(jsint)d));
}

static bool isPromoteUint(LIns *i)
{
    jsdouble d;
    return i->isop(LIR_u2f) || (i->isconstq() && ((d = i->constvalf()) == (jsdouble)(jsuint)d));
}

static bool isPromote(LIns *i)
{
    return isPromoteInt(i) || isPromoteUint(i);;
}

class FuncFilter: public LirWriter
{
    TraceRecorder& recorder;
public:
    FuncFilter(LirWriter *out, TraceRecorder& _recorder):
        LirWriter(out), recorder(_recorder)
    {
    }

    LInsp ins1(LOpcode v, LInsp s0)
    {
        switch (v) {
          case LIR_i2f:
            if (s0->oprnd1()->isCall() && s0->imm8() == F_doubleToInt32)
                return callArgN(s0->oprnd1(), 1);
            break;
          case LIR_u2f:
            if (s0->oprnd1()->isCall() && s0->imm8() == F_doubleToUint32)
                return callArgN(s0->oprnd1(), 1);
            break;
          case LIR_fneg:
              if (isPromoteInt(s0)) {
                  LIns* result = out->ins1(LIR_neg, demote(out, s0));
                  out->insGuard(LIR_xt, out->ins1(LIR_ov, result), recorder.snapshot());
                  return out->ins1(LIR_i2f, result);
              }                  
              break;
          default:;
        }
        return out->ins1(v, s0);
    }

    LInsp ins2(LOpcode v, LInsp s1, LInsp s0)
    {
        if (s0 == s1 && v == LIR_feq) {
            if (isPromote(s0)) {
                // double(int) and double(uint) cannot be nan
                return insImm(1);
            }
            if (s0->isop(LIR_fmul) || s0->isop(LIR_fsub) || s0->isop(LIR_fadd)) {
                LInsp lhs = s0->oprnd1();
                LInsp rhs = s0->oprnd2();
                if (isPromote(lhs) && isPromote(rhs)) {
                    // add/sub/mul promoted ints can't be nan
                    return insImm(1);
                }
            }
        } else if (LIR_feq <= v && v <= LIR_fge) {
            if (isPromoteInt(s0) && isPromoteInt(s1)) {
                // demote fcmp to cmp
                v = LOpcode(v + (LIR_eq - LIR_feq));
                return out->ins2(v, demote(out, s1), demote(out, s0));
            } else if (isPromoteUint(s0) && isPromoteUint(s1)) {
                // uint compare
                v = LOpcode(v + (LIR_eq - LIR_feq));
                if (v != LIR_eq)
                    v = LOpcode(v + (LIR_ult - LIR_lt)); // cmp -> ucmp
                return out->ins2(v, demote(out, s1), demote(out, s0));
            }
        } else if (v == LIR_fadd || v == LIR_fsub || v == LIR_fmul) {
            if (isPromoteInt(s0) && isPromoteInt(s1)) {
                // demote fop to op
                v = (LOpcode)((int)v & ~LIR64);
                LIns* result = out->ins2(v, demote(out, s1), demote(out, s0));
                out->insGuard(LIR_xt, out->ins1(LIR_ov, result), recorder.snapshot());
                return out->ins1(LIR_i2f, result);
            }
        }
        return out->ins2(v, s1, s0);
    }

    LInsp insCall(int32_t fid, LInsp args[])
    {
        LInsp s0 = args[0];
        switch (fid) {
          case F_doubleToInt32:
            if (s0->isconstq())
                return out->insImm(js_DoubleToECMAInt32(s0->constvalf()));
            if (s0->isop(LIR_fadd) || s0->isop(LIR_fsub) || s0->isop(LIR_fmul)) {
                LInsp lhs = s0->oprnd1();
                LInsp rhs = s0->oprnd2();
                if (isPromote(lhs) && isPromote(rhs)) {
                    LOpcode op = LOpcode(s0->opcode() & ~LIR64);
                    return out->ins2(op, demote(out, lhs), demote(out, rhs));
                }
            }
            if (s0->isop(LIR_i2f) || s0->isop(LIR_u2f)) {
                return s0->oprnd1();
            }
            break;
          case F_BoxDouble:
            JS_ASSERT(s0->isQuad());
            if (s0->isop(LIR_i2f)) {
                LIns* args2[] = { s0->oprnd1(), args[1] };
                return out->insCall(F_BoxInt32, args2);
            }
            break;
        }
        return out->insCall(fid, args);
    }
};

/* In debug mode vpname contains a textual description of the type of the
   slot during the forall iteration over al slots. */
#ifdef DEBUG
#define DEF_VPNAME          char* vpname; unsigned vpnum
#define SET_VPNAME(name)    do { vpname = name; vpnum = 0; } while(0)
#define INC_VPNUM()         do { ++vpnum; } while(0)
#else
#define DEF_VPNAME          do {} while (0)
#define vpname ""
#define vpnum 0
#define SET_VPNAME(name)    ((void)0)
#define INC_VPNUM()         ((void)0)
#endif

/* This macro can be used to iterate over all slots in currently pending
   frames that make up the native frame, including global variables and
   frames consisting of rval, args, vars, and stack (except for the top-
   level frame which does not have args or vars. */
#define FORALL_SLOTS_IN_PENDING_FRAMES(cx, entryFrame, currentFrame, code)    \
    JS_BEGIN_MACRO                                                            \
        DEF_VPNAME;                                                           \
        /* find the global frame */                                           \
        JSStackFrame* global = entryFrame;                                    \
        while (global->down)                                                  \
            global = global->down;                                            \
        JSObject* gvarobj = global->varobj;                                   \
        unsigned n;                                                           \
        jsval* vp;                                                            \
        JSAtom** atoms = entryFrame->script->atomMap.vector;                  \
        unsigned natoms = entryFrame->script->atomMap.length;                 \
        SET_VPNAME("global");                                                 \
        for (n = 0; n < natoms; ++n) {                                        \
            JSAtom* atom = atoms[n];                                          \
            if (!ATOM_IS_STRING(atom))                                        \
                continue;                                                     \
            jsid id = ATOM_TO_JSID(atom);                                     \
            JSObject* obj2;                                                   \
            JSScopeProperty* sprop;                                           \
            if (!js_LookupProperty(cx, gvarobj, id, &obj2, (JSProperty**)&sprop)); \
                continue;                                                     \
            JS_ASSERT(obj2 == gvarobj);                                       \
            if (!SPROP_HAS_STUB_GETTER(sprop) ||                              \
                !SPROP_HAS_STUB_SETTER(sprop))                                \
                continue;                                                     \
            vp = &STOBJ_GET_SLOT(gvarobj, sprop->slot);                       \
            { code; }                                                         \
            INC_VPNUM();                                                      \
        }                                                                     \
        /* count the number of pending frames */                              \
        unsigned frames = 0;                                                  \
        JSStackFrame* fp = currentFrame;                                      \
        for (;; fp = fp->down) { ++frames; if (fp == entryFrame) break; };    \
        /* stack them up since we want forward order (this should be fast */  \
        /* now, since the previous loop prefetched everything for us and  */  \
        /* the list tends to be short anyway [1-3 frames]).               */  \
        JSStackFrame** fstack = (JSStackFrame **)alloca(frames * sizeof (JSStackFrame *)); \
        JSStackFrame** fspstop = &fstack[frames];                             \
        JSStackFrame** fsp = fspstop-1;                                       \
        fp = currentFrame;                                                    \
        for (;; fp = fp->down) { *fsp-- = fp; if (fp == entryFrame) break; }  \
        for (fsp = fstack; fsp < fspstop; ++fsp) {                            \
            JSStackFrame* f = *fsp;                                           \
            jsval* vpstop;                                                    \
            SET_VPNAME("rval");                                               \
            vp = &f->rval; code;                                              \
            if (f->callee) {                                                  \
                SET_VPNAME("argv");                                           \
                vp = &f->argv[0]; vpstop = &f->argv[f->argc];                 \
                while (vp < vpstop) { code; ++vp; INC_VPNUM(); }              \
                SET_VPNAME("vars");                                           \
                vp = &f->vars[0]; vpstop = &f->vars[f->nvars];                \
                while (vp < vpstop) { code; ++vp; INC_VPNUM(); }              \
            }                                                                 \
            SET_VPNAME("stack");                                              \
            vp = f->spbase; vpstop = f->regs->sp;                             \
            while (vp < vpstop) { code; ++vp; INC_VPNUM(); }                  \
        }                                                                     \
    JS_END_MACRO

class ExitFilter: public LirWriter
{
    TraceRecorder& recorder;
public:
    ExitFilter(LirWriter *out, TraceRecorder& _recorder):
        LirWriter(out), recorder(_recorder)
    {
    }

    /* Determine the type of a store by looking at the current type of the actual value the
       interpreter is using. For numbers we have to check what kind of store we used last
       (integer or double) to figure out what the side exit show reflect in its typemap. */
    int getStoreType(jsval& v) {
        LIns* i = recorder.get(&v);
        int t = isNumber(v)
                ? (isPromoteInt(i) ? JSVAL_INT : JSVAL_DOUBLE)
                : JSVAL_TAG(v);
         return t;
    }

    /* Write out a type map for the current scopes and all outer scopes,
       up until the entry scope. */
    void
    buildExitMap(JSContext* cx, JSStackFrame* entryFrame, JSStackFrame* currentFrame, uint8* m)
    {
        FORALL_SLOTS_IN_PENDING_FRAMES(cx, entryFrame, currentFrame,
            *m++ = getStoreType(*vp));
    }

    virtual LInsp insGuard(LOpcode v, LIns *c, SideExit *x) {
        buildExitMap(recorder.getContext(), recorder.getFp(), recorder.getFp(), x->typeMap);
        return out->insGuard(v, c, x);
    }

    /* Sink all type casts into the stack into the side exit by simply storing the original
       (uncasted) value. Each guard generates the side exit map based on the types of the
       last stores to every stack location, so its safe to not perform them on-trace. */
    virtual LInsp insStore(LIns* value, LIns* base, LIns* disp) {
        if (base == recorder.getFragment()->sp && isPromoteInt(value))
            value = demote(out, value);
        return out->insStore(value, base, disp);
    }

    virtual LInsp insStorei(LIns* value, LIns* base, int32_t d) {
        if (base == recorder.getFragment()->sp && isPromoteInt(value))
            value = demote(out, value);
        return out->insStorei(value, base, d);
    }
};

TraceRecorder::TraceRecorder(JSContext* cx, Fragmento* fragmento, Fragment* _fragment)
{
    this->cx = cx;
    JSStackFrame* global = cx->fp;
    while (global->down)
        global = global->down;
    this->global = global;
    this->fragment = _fragment;
    entryFrame = cx->fp;
    entryRegs.pc = entryFrame->regs->pc;
    entryRegs.sp = entryFrame->regs->sp;
    this->atoms = cx->fp->script->atomMap.vector;

#ifdef DEBUG
    printf("recording starting from %s:%u\n", cx->fp->script->filename, 
           js_PCToLineNumber(cx, cx->fp->script, entryRegs.pc));
#endif

    fragment->calldepth = 0;
    lirbuf = new (&gc) LirBuffer(fragmento, builtins);
    fragment->lirbuf = lirbuf;
    lir = lir_buf_writer = new (&gc) LirBufWriter(lirbuf);
#ifdef DEBUG
    lirbuf->names = new (&gc) LirNameMap(&gc, builtins, fragmento->labels);
    lir = verbose_filter = new (&gc) VerboseWriter(&gc, lir, lirbuf->names);
#endif
    lir = cse_filter = new (&gc) CseFilter(lir, &gc);
    lir = expr_filter = new (&gc) ExprFilter(lir);
    lir = exit_filter = new (&gc) ExitFilter(lir, *this);
    lir = func_filter = new (&gc) FuncFilter(lir, *this);
    lir->ins0(LIR_trace);
    if (fragment->vmprivate == NULL) {
        /* generate the entry map and stash it in the trace */
        unsigned entryNativeFrameSlots = nativeFrameSlots(entryFrame, entryRegs);
        LIns* data = lir_buf_writer->skip(sizeof(*fragmentInfo) -
                sizeof(fragmentInfo->typeMap) + entryNativeFrameSlots * sizeof(char));
        fragmentInfo = (VMFragmentInfo*)data->payload();
        fragmentInfo->entryNativeFrameSlots = entryNativeFrameSlots;
        fragmentInfo->nativeStackBase = (entryNativeFrameSlots -
                (entryRegs.sp - entryFrame->spbase)) * sizeof(double);
        fragmentInfo->maxNativeFrameSlots = entryNativeFrameSlots;
        /* build the entry type map */
        uint8* m = fragmentInfo->typeMap;
        /* remember the coerced type of each active slot in the type map */
        FORALL_SLOTS_IN_PENDING_FRAMES(cx, entryFrame, entryFrame,
            *m++ = getCoercedType(*vp));
    } else {
        fragmentInfo = (VMFragmentInfo*)fragment->vmprivate;
    }
    fragment->vmprivate = fragmentInfo;
    fragment->state = lir->insImm8(LIR_param, Assembler::argRegs[0], 0);
    fragment->param1 = lir->insImm8(LIR_param, Assembler::argRegs[1], 0);
    fragment->sp = lir->insLoadi(fragment->state, offsetof(InterpState, sp));
    cx_ins = lir->insLoadi(fragment->state, offsetof(InterpState, cx));
#ifdef DEBUG
    lirbuf->names->addName(fragment->state, "state");
    lirbuf->names->addName(fragment->sp, "sp");
    lirbuf->names->addName(cx_ins, "cx");
#endif

    uint8* m = fragmentInfo->typeMap;
    FORALL_SLOTS_IN_PENDING_FRAMES(cx, entryFrame, entryFrame,
        import(vp, *m, vpname, vpnum);
        m++
    );

    recompileFlag = false;
}

TraceRecorder::~TraceRecorder()
{
#ifdef DEBUG
    delete lirbuf->names;
    delete verbose_filter;
#endif
    delete cse_filter;
    delete expr_filter;
    delete exit_filter;
    delete func_filter;
    delete lir_buf_writer;
}

/* Determine the current call depth (starting with the entry frame.) */
unsigned
TraceRecorder::getCallDepth() const
{
    JSStackFrame* fp = cx->fp;
    unsigned depth = 0;
    while (fp != entryFrame) {
        ++depth;
        fp = fp->down;
    }
    return depth;
}

/* Find the frame that this address belongs to (if any). */
JSStackFrame*
TraceRecorder::findFrame(jsval* p) const
{
    jsval* vp = (jsval*) p;
    JSStackFrame* fp = cx->fp;
    for (;;) {
        // FIXME: fixing bug 441686 collapses the last two tests here
        if (vp == p ||
            size_t(vp - fp->argv) < fp->argc ||
            size_t(vp - fp->vars) < fp->nvars ||
            size_t(vp - fp->spbase) < fp->script->depth) {
            return fp;
        }
        if (fp == entryFrame)
           return NULL;
        fp = fp->down;
    }
    JS_NOT_REACHED("findFrame");
}

/* Determine whether an address is part of a currently active frame (or the global scope). */
bool
TraceRecorder::onFrame(jsval* p) const
{
    return isGlobal(p) || findFrame(p) != NULL;
}

/* Determine whether an address points to a global variable (gvar). */
bool
TraceRecorder::isGlobal(jsval* p) const
{
    JSObject* varobj = global->varobj;

    /* has to be in either one of the fslots or dslots of varobj */
    if (size_t(p - varobj->fslots) < JS_INITIAL_NSLOTS)
        return true;
    return varobj->dslots &&
           size_t(p - varobj->dslots) < size_t(varobj->dslots[-1] - JS_INITIAL_NSLOTS);
}

/* Calculate the total number of native frame slots we need from this frame
   all the way back to the entry frame, including the current stack usage. */
unsigned
TraceRecorder::nativeFrameSlots(JSStackFrame* fp, JSFrameRegs& regs) const
{
    unsigned slots = 0;
    JSObject* gvarobj = global->varobj;
    unsigned n;
    JSAtom** atoms = entryFrame->script->atomMap.vector;
    unsigned natoms = entryFrame->script->atomMap.length;
    for (n = 0; n < natoms; ++n) {
        JSAtom* atom = atoms[n];
        if (!ATOM_IS_STRING(atom))
            continue;
        jsid id = ATOM_TO_JSID(atom);
        JSObject* obj2;
        JSScopeProperty* sprop;
        if (!js_LookupProperty(cx, gvarobj, id, &obj2, (JSProperty**)&sprop))
            continue;
        if (!SPROP_HAS_STUB_GETTER(sprop) ||
            !SPROP_HAS_STUB_SETTER(sprop))
            continue;
        ++slots;
    }
    for (;;) {
        slots += 1/*rval*/ + (regs.sp - fp->spbase);
        if (fp->callee)
            slots += fp->argc + fp->nvars;
        if (fp == entryFrame)
            return slots;
        fp = fp->down;
    }
    JS_NOT_REACHED("nativeFrameSlots");
}

/* Determine the offset in the native frame (marshal) for an address
   that is part of a currently active frame. */
size_t
TraceRecorder::nativeFrameOffset(jsval* p) const
{
    JSStackFrame* currentFrame = cx->fp;
    size_t offset = 0;
    FORALL_SLOTS_IN_PENDING_FRAMES(cx, entryFrame, currentFrame,
        if (vp == p) return offset;
        offset += sizeof(double)
    );
    /* if its not in a pending frame, it must be on the stack of the current frame above
       sp but below script->depth */
    JS_ASSERT(size_t(p - currentFrame->regs->sp) < currentFrame->script->depth);
    offset += size_t(p - currentFrame->regs->sp) * sizeof(double);
    return offset;
}

/* Track the maximum number of native frame slots we need during
   execution. */
void
TraceRecorder::trackNativeFrameUse(unsigned slots)
{
    if (slots > fragmentInfo->maxNativeFrameSlots)
        fragmentInfo->maxNativeFrameSlots = slots;
}

/* Unbox a jsval into a slot. Slots are wide enough to hold double values
   directly (instead of storing a pointer to them). */
static bool
unbox_jsval(jsval v, uint8 t, double* slot)
{
    jsuint type = TYPEMAP_GET_TYPE(t);
    if (type == TYPEMAP_TYPE_ANY) {
        verbose_only(printf("any ");)
        return true;
    }
    if (type == JSVAL_INT) {
        jsint i;
        if (JSVAL_IS_INT(v))
            *(jsint*)slot = JSVAL_TO_INT(v);
        else if (JSVAL_IS_DOUBLE(v) && JSDOUBLE_IS_INT(*JSVAL_TO_DOUBLE(v), i))
            *(jsint*)slot = i;
        else {
            verbose_only(printf("int != tag%d(value=%d) ", JSVAL_TAG(v), v);)
            return false;
        }
        verbose_only(printf("int<%d> ", i);)
        return true;
    }
    if (type == JSVAL_DOUBLE) {
        jsdouble d;
        if (JSVAL_IS_INT(v))
            d = JSVAL_TO_INT(v);
        else if (JSVAL_IS_DOUBLE(v))
            d = *JSVAL_TO_DOUBLE(v);
        else {
            verbose_only(printf("double != tag%d ", JSVAL_TAG(v));)
            return false;
        }
        *(jsdouble*)slot = d;
        verbose_only(printf("double<%g> ", d);)
        return true;
    }
    if (JSVAL_TAG(v) != type) {
        verbose_only(printf("%d != tag%d ", type, JSVAL_TAG(v));)
        return false;
    }
    switch (JSVAL_TAG(v)) {
      case JSVAL_BOOLEAN:
        *(bool*)slot = JSVAL_TO_BOOLEAN(v);
        verbose_only(printf("boolean<%d> ", *(bool*)slot);)
        break;
      case JSVAL_STRING:
        *(JSString**)slot = JSVAL_TO_STRING(v);
        verbose_only(printf("string<%p> ", *(JSString**)slot);)
        break;
      default:
        JS_ASSERT(JSVAL_IS_OBJECT(v));
        *(JSObject**)slot = JSVAL_TO_OBJECT(v);
        verbose_only(printf("object<%p:%s> ", JSVAL_TO_OBJECT(v),
                            JSVAL_IS_NULL(v)
                            ? "null"
                            : STOBJ_GET_CLASS(JSVAL_TO_OBJECT(v))->name);)
    }
    return true;
}

/* Box a value from the native stack back into the jsval format. Integers
   that are too large to fit into a jsval are automatically boxed into
   heap-allocated doubles. */
static bool
box_jsval(JSContext* cx, jsval& v, uint8 t, double* slot)
{
    jsuint type = TYPEMAP_GET_TYPE(t);
    if (type == TYPEMAP_TYPE_ANY) {
        verbose_only(printf("any ");)
        return true;
    }
    jsint i;
    jsdouble d;
    switch (type) {
      case JSVAL_BOOLEAN:
        v = BOOLEAN_TO_JSVAL(*(bool*)slot);
        verbose_only(printf("boolean<%d> ", *(bool*)slot);)
        break;
      case JSVAL_INT:
        i = *(jsint*)slot;
        verbose_only(printf("int<%d> ", i);)
      store_int:
        if (INT_FITS_IN_JSVAL(i)) {
            v = INT_TO_JSVAL(i);
            break;
        }
        d = (jsdouble)i;
        goto store_double;
      case JSVAL_DOUBLE:
        d = *slot;
        verbose_only(printf("double<%g> ", d);)
        if (JSDOUBLE_IS_INT(d, i))
            goto store_int;
      store_double:
        /* Its safe to trigger the GC here since we rooted all strings/objects and all the
           doubles we already processed. */
        return js_NewDoubleInRootedValue(cx, d, &v);
      case JSVAL_STRING:
        v = STRING_TO_JSVAL(*(JSString**)slot);
        verbose_only(printf("string<%p> ", *(JSString**)slot);)
        break;
      default:
        JS_ASSERT(t == JSVAL_OBJECT);
        v = OBJECT_TO_JSVAL(*(JSObject**)slot);
        verbose_only(printf("object<%p> ", *(JSObject**)slot);)
        break;
    }
    return true;
}

/* Attempt to unbox the given JS frame into a native frame, checking along the way that the
   supplied typemap holds. */
static bool
unbox(JSContext* cx, JSStackFrame* entryFrame, JSStackFrame* currentFrame, uint8* map, double* native)
{
    verbose_only(printf("unbox native@%p ", native);)
    double* np = native;
    uint8* mp = map;
    FORALL_SLOTS_IN_PENDING_FRAMES(cx, entryFrame, currentFrame,
        if (!unbox_jsval(*vp, *mp, np))
            return false;
        ++mp; ++np;
    );
    verbose_only(printf("\n");)
    return true;
}

/* Box the given native frame into a JS frame. This only fails due to a hard error
   (out of memory for example). */
static bool
box(JSContext* cx, JSStackFrame* entryFrame, JSStackFrame* currentFrame, uint8* map, double* native)
{
    verbose_only(printf("box native@%p ", native);)
    double* np = native;
    uint8* mp = map;
    /* Root all string and object references first (we don't need to call the GC for this). */
    FORALL_SLOTS_IN_PENDING_FRAMES(cx, entryFrame, currentFrame,
        if ((*mp == JSVAL_STRING || *mp == JSVAL_OBJECT) && !box_jsval(cx, *vp, *mp, np))
            return false;
        ++mp; ++np
    );
    /* Now do this again but this time for all values (properly quicker than actually checking
       the type and excluding strings and objects). The GC might kick in when we store doubles,
       but everything is rooted now (all strings/objects and all doubles we already boxed). */
    np = native;
    mp = map;
    FORALL_SLOTS_IN_PENDING_FRAMES(cx, entryFrame, currentFrame,
        if (!box_jsval(cx, *vp, *mp, np))
            return false;
        ++mp; ++np
    );
    verbose_only(printf("\n");)
    return true;
}

/* Emit load instructions onto the trace that read the initial stack state. */
void
TraceRecorder::import(jsval* p, uint8& t, char *prefix, int index)
{
    JS_ASSERT(TYPEMAP_GET_TYPE(t) != TYPEMAP_TYPE_ANY);
    JS_ASSERT(onFrame(p));
    LIns* ins;
    /* Calculate the offset of this slot relative to the entry stack-pointer value of the
       native stack. Arguments and locals are to the left of the stack pointer (offset
       less than 0). Stack cells start at offset 0. Ed defined the semantics of the stack,
       not me, so don't blame the messenger. */
    ptrdiff_t offset = -fragmentInfo->nativeStackBase + nativeFrameOffset(p) + 8;
    if (TYPEMAP_GET_TYPE(t) == JSVAL_INT) { /* demoted */
        JS_ASSERT(isInt32(*p));
        /* Ok, we have a valid demotion attempt pending, so insert an integer
           read and promote it to double since all arithmetic operations expect
           to see doubles on entry. The first op to use this slot will emit a
           f2i cast which will cancel out the i2f we insert here. */
        ins = lir->ins1(LIR_i2f, lir->insLoadi(fragment->sp, offset));
    } else {
        JS_ASSERT(isNumber(*p) == (TYPEMAP_GET_TYPE(t) == JSVAL_DOUBLE));
        ins = lir->insLoad(t == JSVAL_DOUBLE ? LIR_ldq : LIR_ld, fragment->sp, offset);
    }
    tracker.set(p, ins);
#ifdef DEBUG
    char name[16];
    JS_ASSERT(strlen(prefix) < 10);
    JS_snprintf(name, sizeof name, "$%s%d", prefix, index);
    lirbuf->names->addName(ins, name);
    static const char* typestr[] = {
            "object", "int", "double", "3", "string", "5", "boolean", "any"
    };  
    printf("import vp=%x name=%s type=%s flags=%d\n", p, name, typestr[t & 7], t >> 3);
#endif
}

/* Update the tracker. If the value is part of any argv/vars/stack of any
   currently active frame (onFrame), then issue a write back store. */
void
TraceRecorder::set(jsval* p, LIns* i)
{
    tracker.set(p, i);
    if (onFrame(p))
        lir->insStorei(i, fragment->sp, -fragmentInfo->nativeStackBase + nativeFrameOffset(p) + 8);
}

LIns*
TraceRecorder::get(jsval* p)
{
    return tracker.get(p);
}

JSContext*
TraceRecorder::getContext() const
{
    return cx;
}

JSStackFrame*
TraceRecorder::getGlobalFrame() const
{
    return global;
}

JSStackFrame*
TraceRecorder::getEntryFrame() const
{
    return entryFrame;
}

JSStackFrame*
TraceRecorder::getFp() const
{
    return cx->fp;
}

JSFrameRegs&
TraceRecorder::getRegs() const
{
    return *cx->fp->regs;
}

Fragment*
TraceRecorder::getFragment() const
{
    return fragment;
}

SideExit*
TraceRecorder::snapshot()
{
    /* generate the entry map and stash it in the trace */
    unsigned slots = nativeFrameSlots(cx->fp, *cx->fp->regs);
    trackNativeFrameUse(slots);
    /* reserve space for the type map, ExitFilter will write it out for us */
    LIns* data = lir_buf_writer->skip(slots * sizeof(uint8));
    /* setup side exit structure */
    memset(&exit, 0, sizeof(exit));
    exit.from = fragment;
    exit.calldepth = getCallDepth();
    exit.sp_adj = (cx->fp->regs->sp - entryRegs.sp) * sizeof(double);
    exit.ip_adj = cx->fp->regs->pc - entryRegs.pc;
    exit.typeMap = (uint8 *)data->payload();
    return &exit;
}

void
TraceRecorder::guard(bool expected, LIns* cond)
{
    lir->insGuard(expected ? LIR_xf : LIR_xt,
                  cond,
                  snapshot());
}

bool
TraceRecorder::checkType(jsval& v, uint8& t)
{
    if (t == TYPEMAP_TYPE_ANY) /* ignore unused slots */
        return true;
    if (isNumber(v)) {
        /* Initially we start out all numbers as JSVAL_DOUBLE in the type map. If we still
           see a number in v, its a valid trace but we might want to ask to demote the
           slot if we know or suspect that its integer. */
        LIns* i = get(&v);
        if (TYPEMAP_GET_TYPE(t) == JSVAL_DOUBLE) {
            if (isInt32(v) && !TYPEMAP_GET_FLAG(t, TYPEMAP_FLAG_DONT_DEMOTE)) {
                /* If the value associated with v via the tracker comes from a i2f operation,
                   we can be sure it will always be an int. If we see INCVAR, we similarly
                   speculate that the result will be int, even though this is not
                   guaranteed and this might cause the entry map to mismatch and thus
                   the trace never to be entered. */
                if (i->isop(LIR_i2f) ||
                        (i->isop(LIR_fadd) && i->oprnd2()->isconstq() &&
                                fabs(i->oprnd2()->constvalf()) == 1.0)) {
#ifdef DEBUG
                    printf("demoting type of an entry slot #%d, triggering re-compilation\n",
                            nativeFrameOffset(&v));
#endif
                    JS_ASSERT(!TYPEMAP_GET_FLAG(t, TYPEMAP_FLAG_DEMOTE) ||
                            TYPEMAP_GET_FLAG(t, TYPEMAP_FLAG_DONT_DEMOTE));
                    TYPEMAP_SET_FLAG(t, TYPEMAP_FLAG_DEMOTE);
                    TYPEMAP_SET_TYPE(t, JSVAL_INT);
                    recompileFlag = true;
                    return true; /* keep going */
                }
            }
            return true;
        }
        /* Looks like we are compiling an integer slot. The recorder always casts to doubles
           after each integer operation, or emits an operation that produces a double right
           away. If we started with an integer, we must arrive here pointing at a i2f cast.
           If not, than demoting the slot didn't work out. Flag the slot to be not
           demoted again. */
        JS_ASSERT(TYPEMAP_GET_TYPE(t) == JSVAL_INT &&
                TYPEMAP_GET_FLAG(t, TYPEMAP_FLAG_DEMOTE) &&
                !TYPEMAP_GET_FLAG(t, TYPEMAP_FLAG_DONT_DEMOTE));
        if (!i->isop(LIR_i2f)) {
#ifdef DEBUG
            printf("demoting type of a slot #%d failed, locking it and re-compiling\n",
                    nativeFrameOffset(&v));
#endif
            TYPEMAP_SET_FLAG(t, TYPEMAP_FLAG_DONT_DEMOTE);
            TYPEMAP_SET_TYPE(t, JSVAL_DOUBLE);
            recompileFlag = true;
            return true; /* keep going, recompileFlag will trigger error when we are done with
                            all the slots */

        }
        JS_ASSERT(isInt32(v));
        /* Looks like we got the final LIR_i2f as we expected. Overwrite the value in that
           slot with the argument of i2f since we want the integer store to flow along
           the loop edge, not the casted value. */
        set(&v, i->oprnd1());
        return true;
    }
    /* for non-number types we expect a precise match of the type */
#ifdef DEBUG
    if (JSVAL_TAG(v) != TYPEMAP_GET_TYPE(t)) {
        printf("Type mismatch: val %c, map %c ", "OID?S?B"[JSVAL_TAG(v)],
               "OID?S?B"[t]);
    }
#endif
    return JSVAL_TAG(v) == TYPEMAP_GET_TYPE(t);
}

/* Make sure that the current values in the given stack frame and all stack frames
   up and including entryFrame are type-compatible with the entry map. */
bool
TraceRecorder::verifyTypeStability(JSStackFrame* entryFrame, JSStackFrame* currentFrame, uint8* m)
{
    FORALL_SLOTS_IN_PENDING_FRAMES(cx, entryFrame, currentFrame,
        if (!checkType(*vp, *m))
            return false;
        ++m
    );
    return !recompileFlag;
}

void
TraceRecorder::closeLoop(Fragmento* fragmento)
{
    if (!verifyTypeStability(entryFrame, cx->fp, fragmentInfo->typeMap)) {
#ifdef DEBUG
        printf("Trace rejected: unstable loop variables.\n");
#endif
        return;
    }
    fragment->lastIns = lir->insGuard(LIR_loop, lir->insImm(1), snapshot());
    compile(fragmento->assm(), fragment);
}

bool
TraceRecorder::loopEdge()
{
    if (cx->fp->regs->pc == entryRegs.pc) {
        closeLoop(JS_TRACE_MONITOR(cx).fragmento);
        return false; /* done recording */
    }
    return false; /* abort recording */
}

void
TraceRecorder::stop()
{
    fragment->blacklist();
}

int
nanojit::StackFilter::getTop(LInsp guard)
{
    return guard->exit()->sp_adj + 8;
}

#if defined NJ_VERBOSE
void
nanojit::LirNameMap::formatGuard(LIns *i, char *out)
{
    uint32_t ip;
    SideExit *x;

    x = (SideExit *)i->exit();
    ip = intptr_t(x->from->ip) + x->ip_adj;
    sprintf(out,
        "%s: %s %s -> %s sp%+d",
        formatRef(i),
        lirNames[i->opcode()],
        i->oprnd1()->isCond() ? formatRef(i->oprnd1()) : "",
        labels->format((void *)ip),
        x->sp_adj
        );
}
#endif

void
nanojit::Assembler::initGuardRecord(LIns *guard, GuardRecord *rec)
{
    SideExit *exit;

    exit = guard->exit();
    rec->calldepth = exit->calldepth;
    rec->exit = exit;
    verbose_only(rec->sid = exit->sid);
}

void
nanojit::Assembler::asm_bailout(LIns *guard, Register state)
{
    SideExit *exit;

    exit = guard->exit();

#if defined(NANOJIT_IA32)
    if (exit->sp_adj)
        ADDmi((int32_t)offsetof(InterpState, sp), state, exit->sp_adj);

    if (exit->ip_adj)
        ADDmi((int32_t)offsetof(InterpState, ip), state, exit->ip_adj);
#elif defined(NANOJIT_ARM)
    NanoAssert(offsetof(avmplus::InterpState,ip) == 0);
    NanoAssert(offsetof(avmplus::InterpState,sp) == 4);
    NanoAssert(offsetof(avmplus::InterpState,cx) == 8);
    RegisterMask ptrs = 0xe; // { R1-R3 }

    SUBi(state,12);
    STMIA(state,ptrs);

    if (exit->sp_adj)       ADDi(R2, exit->sp_adj);
    if (exit->ip_adj)       ADDi(R1, exit->ip_adj);

    SUBi(state,12);
    LDMIA(state,ptrs);
#endif
}

void
js_DeleteRecorder(JSContext* cx)
{
    JSTraceMonitor* tm = &JS_TRACE_MONITOR(cx);
    delete tm->recorder;
    tm->recorder = NULL;
}

#define HOTLOOP1 10
#define HOTLOOP2 13
#define HOTLOOP3 37

bool
js_LoopEdge(JSContext* cx)
{
    JSTraceMonitor* tm = &JS_TRACE_MONITOR(cx);

    /* is the recorder currently active? */
    if (tm->recorder) {
#ifdef JS_THREADSAFE
        /* XXX should this test not be earlier, to avoid even recording? */
        if (OBJ_SCOPE(tm->recorder->getGlobalFrame()->varobj)->title.ownercx != cx) {
#ifdef DEBUG
            printf("Global object not owned by this context.\n");
#endif
            return false; /* we stay away from shared global objects */
        }
#endif

        if (tm->recorder->loopEdge())
            return true; /* keep recording */
        js_DeleteRecorder(cx);
        return false; /* done recording */
    }

    Fragment* f = tm->fragmento->getLoop(cx->fp->regs->pc);
    if (!f->code()) {
        int hits = ++f->hits();
        if (!f->isBlacklisted() && hits >= HOTLOOP1) {
            if (hits == HOTLOOP1 || hits == HOTLOOP2 || hits == HOTLOOP3) {
                tm->recorder = new (&gc) TraceRecorder(cx, tm->fragmento, f);
                return true; /* start recording */
            }
            if (hits > HOTLOOP3)
                f->blacklist();
        }
        return false;
    }

    /* execute previously recorded trace */
    VMFragmentInfo* fi = (VMFragmentInfo*)f->vmprivate;
    double* native = (double *)alloca((fi->maxNativeFrameSlots+1) * sizeof(double));
#ifdef DEBUG
    *(uint64*)&native[fi->maxNativeFrameSlots] = 0xdeadbeefdeadbeefLL;
#endif
    if (!unbox(cx, cx->fp, cx->fp, fi->typeMap, native)) {
#ifdef DEBUG
        printf("typemap mismatch, skipping trace.\n");
#endif
        return false;
    }
    double* entry_sp = &native[fi->nativeStackBase/sizeof(double) +
                               (cx->fp->regs->sp - cx->fp->spbase - 1)];
    InterpState state;
    state.ip = cx->fp->regs->pc;
    state.sp = (void*)entry_sp;
    state.cx = cx;
    union { NIns *code; GuardRecord* (FASTCALL *func)(InterpState*, Fragment*); } u;
    u.code = f->code();
#if defined(DEBUG) && defined(AVMPLUS_IA32)
    printf("entering trace at %s:%u, sp=%p\n", 
           cx->fp->script->filename, js_PCToLineNumber(cx, cx->fp->script, cx->fp->regs->pc),
           state.sp);
    uint64 start = rdtsc();
#endif
    GuardRecord* lr = u.func(&state, NULL);
    cx->fp->regs->sp += (double*)state.sp - entry_sp;
    cx->fp->regs->pc = (jsbytecode*)state.ip;
#if defined(DEBUG) && defined(AVMPLUS_IA32)
    printf("leaving trace at %s:%u, sp=%p, cycles=%llu\n", 
           cx->fp->script->filename, js_PCToLineNumber(cx, cx->fp->script, cx->fp->regs->pc), 
           state.sp,
           (rdtsc() - start));
#endif
    box(cx, cx->fp, cx->fp, lr->exit->typeMap, native);
#ifdef DEBUG
    JS_ASSERT(*(uint64*)&native[fi->maxNativeFrameSlots] == 0xdeadbeefdeadbeefLL);
#endif

    return false; /* continue with regular interpreter */
}

void
js_AbortRecording(JSContext* cx, const char* reason)
{
#ifdef DEBUG
    printf("Abort recording: %s.\n", reason);
#endif
    JS_TRACE_MONITOR(cx).recorder->stop();
    js_DeleteRecorder(cx);
}

extern void
js_InitJIT(JSContext* cx)
{
    JSTraceMonitor* tm = &JS_TRACE_MONITOR(cx);
    if (!tm->fragmento) {
        Fragmento* fragmento = new (&gc) Fragmento(core);
#ifdef DEBUG
        fragmento->labels = new (&gc) LabelMap(core, NULL);
#endif
        fragmento->assm()->setCallTable(builtins);
        fragmento->pageFree(fragmento->pageAlloc()); // FIXME: prime page cache
        tm->fragmento = fragmento;
    }
}

jsval&
TraceRecorder::argval(unsigned n) const
{
    JS_ASSERT(n < cx->fp->argc);
    return cx->fp->argv[n];
}

jsval&
TraceRecorder::varval(unsigned n) const
{
    JS_ASSERT(n < cx->fp->nvars);
    return cx->fp->vars[n];
}

jsval&
TraceRecorder::stackval(int n) const
{
    jsval* sp = cx->fp->regs->sp;
    JS_ASSERT(size_t((sp + n) - cx->fp->spbase) < cx->fp->script->depth);
    return sp[n];
}

LIns*
TraceRecorder::arg(unsigned n)
{
    return get(&argval(n));
}

void
TraceRecorder::arg(unsigned n, LIns* i)
{
    set(&argval(n), i);
}

LIns*
TraceRecorder::var(unsigned n)
{
    return get(&varval(n));
}

void
TraceRecorder::var(unsigned n, LIns* i)
{
    set(&varval(n), i);
}

LIns*
TraceRecorder::stack(int n)
{
    return get(&stackval(n));
}

void
TraceRecorder::stack(int n, LIns* i)
{
    set(&stackval(n), i);
}

LIns* TraceRecorder::f2i(LIns* f)
{
    return lir->insCall(F_doubleToInt32, &f);
}

bool TraceRecorder::ifop()
{
    jsval& v = stackval(-1);
    if (JSVAL_IS_BOOLEAN(v)) {
        guard(!JSVAL_TO_BOOLEAN(v), lir->ins_eq0(get(&v)));
    } else {
        return false;
    }
    return true;
}

bool
TraceRecorder::inc(jsval& v, jsint incr, bool pre)
{
    if (isNumber(v)) {
        LIns* before = get(&v);
        LIns* after;
        jsdouble d = (jsdouble)incr;
        after = lir->ins2(LIR_fadd, before, lir->insImmq(*(uint64_t*)&d));
        set(&v, after);

        const JSCodeSpec& cs = js_CodeSpec[*cx->fp->regs->pc];
        JS_ASSERT(cs.ndefs == 1);
        stack(cs.nuses, pre ? after : before);
        return true;
    }
    return false;
}

bool
TraceRecorder::cmp(LOpcode op, bool negate)
{
    jsval& r = stackval(-1);
    jsval& l = stackval(-2);
    if (isNumber(l) && isNumber(r)) {
        LIns* x = lir->ins2(op, get(&l), get(&r));
        if (negate)
            x = lir->ins_eq0(x);
        bool cond;
        switch (op) {
          case LIR_flt:
            cond = asNumber(l) < asNumber(r);
            break;
          case LIR_fgt:
            cond = asNumber(l) > asNumber(r);
            break;
          case LIR_fle:
            cond = asNumber(l) <= asNumber(r);
            break;
          case LIR_fge:
            cond = asNumber(l) >= asNumber(r);
            break;
          default:
            JS_ASSERT(op == LIR_feq);
            cond = (asNumber(l) == asNumber(r)) ^ negate;
            break;
        }
        /* The interpreter fuses comparisons and the following branch,
           so we have to do that here as well. */
        if (cx->fp->regs->pc[1] == JSOP_IFEQ || cx->fp->regs->pc[1] == JSOP_IFNE)
            guard(cond, x);
        /* We update the stack after the guard. This is safe since
           the guard bails out at the comparison and the interpreter
           will this re-execute the comparison. This way the
           value of the condition doesn't have to be calculated and
           saved on the stack in most cases. */
        set(&l, x);
        return true;
    }
    return false;
}

bool
TraceRecorder::unary(LOpcode op)
{
    jsval& v = stackval(-1);
    bool intop = !(op & LIR64);
    if (isNumber(v)) {
        LIns* a = get(&v);
        if (intop)
            a = f2i(a);
        a = lir->ins1(op, a);
        if (intop)
            a = lir->ins1(LIR_i2f, a);
        set(&v, a);
        return true;
    }
    return false;
}

bool
TraceRecorder::binary(LOpcode op)
{
    jsval& r = stackval(-1);
    jsval& l = stackval(-2);
    bool intop = !(op & LIR64);
    if (isNumber(l) && isNumber(r)) {
        LIns* a = get(&l);
        LIns* b = get(&r);
        if (intop) {
            a = lir->insCall(op == LIR_ush ? F_doubleToUint32 : F_doubleToInt32, &a);
            b = f2i(b);
        }
        a = lir->ins2(op, a, b);
        if (intop)
            a = lir->ins1(op == LIR_ush ? LIR_u2f : LIR_i2f, a);
        set(&l, a);
        return true;
    }
    return false;
}

bool
TraceRecorder::map_is_native(JSObjectMap* map, LIns* map_ins)
{
    LIns* ops = lir->insLoadi(map_ins, offsetof(JSObjectMap, ops));
    if (map->ops == &js_ObjectOps) {
        guard(true, lir->ins2(LIR_eq, ops, lir->insImmPtr(&js_ObjectOps)));
        return true;
    }
    LIns* n = lir->insLoadi(ops, offsetof(JSObjectOps, newObjectMap));
    if (map->ops->newObjectMap == js_ObjectOps.newObjectMap) {
        guard(true, lir->ins2(LIR_eq, n, lir->insImmPtr((void*)js_ObjectOps.newObjectMap)));
        return true;
    }
    ABORT_TRACE("non-native map");
}

bool
TraceRecorder::test_property_cache(JSObject* obj, LIns* obj_ins, JSObject*& obj2,
                                   JSPropCacheEntry*& entry)
{
    LIns* map_ins = lir->insLoadi(obj_ins, offsetof(JSObject, map));
    if (!map_is_native(obj->map, map_ins))
        return false;

    JSAtom* atom;
    PROPERTY_CACHE_TEST(cx, cx->fp->regs->pc, obj, obj2, entry, atom);
    if (atom)
        ABORT_TRACE("PC miss");

    if (PCVCAP_TAG(entry->vcap == 1))
        ABORT_TRACE("PC hit in prototype"); // need to look in the prototype, NYI

    if (OBJ_SCOPE(obj)->object != obj)
        ABORT_TRACE("obj not scope owner"); // need to normalize to the owner of the shared scope, NYI

    LIns* shape_ins = lir->insLoadi(map_ins, offsetof(JSScope, shape));
#ifdef DEBUG
    lirbuf->names->addName(shape_ins, "shape");
#endif
    guard(true, lir->ins2i(LIR_eq, shape_ins, OBJ_SCOPE(obj)->shape));
    return true;
}

bool
TraceRecorder::test_property_cache_direct_slot(JSObject* obj, LIns* obj_ins, uint32& slot)
{
    JSObject* obj2;
    JSPropCacheEntry* entry;

    /*
     * Property cache ensures that we are dealing with an existing property,
     * and guards the shape for us.
     */
    if (!test_property_cache(obj, obj_ins, obj2, entry))
        return false;

    /* Handle only gets and sets on the directly addressed object. */
    if (obj2 != obj)
        ABORT_TRACE("PC hit on prototype chain");

    /* Don't trace setter calls, our caller wants a direct slot. */
    if (PCVAL_IS_SPROP(entry->vword)) {
        JS_ASSERT(js_CodeSpec[*cx->fp->regs->pc].format & JOF_SET);
        JSScopeProperty* sprop = PCVAL_TO_SPROP(entry->vword);

        if (!SPROP_HAS_STUB_SETTER(sprop))
            ABORT_TRACE("non-stub setter");
        if (!SPROP_HAS_VALID_SLOT(sprop, OBJ_SCOPE(obj)))
            ABORT_TRACE("no valid slot");
        slot = sprop->slot;
    } else {
        if (!PCVAL_IS_SLOT(entry->vword))
            ABORT_TRACE("PCE is not a slot");
        slot = PCVAL_TO_SLOT(entry->vword);
    }
    return true;
}

void
TraceRecorder::stobj_set_slot(LIns* obj_ins, unsigned slot, LIns*& dslots_ins, LIns* v_ins)
{
    if (slot < JS_INITIAL_NSLOTS) {
        lir->insStorei(v_ins,
                       obj_ins,
                       offsetof(JSObject, fslots) + slot * sizeof(jsval));
    } else {
        if (!dslots_ins)
            dslots_ins = lir->insLoadi(obj_ins, offsetof(JSObject, dslots));
        lir->insStorei(v_ins,
                       dslots_ins,
                       (slot - JS_INITIAL_NSLOTS) * sizeof(jsval));
    }
}

LIns*
TraceRecorder::stobj_get_slot(LIns* obj_ins, unsigned slot, LIns*& dslots_ins)
{
    if (slot < JS_INITIAL_NSLOTS) {
        return lir->insLoadi(obj_ins,
                             offsetof(JSObject, fslots) + slot * sizeof(jsval));
    }

    if (!dslots_ins)
        dslots_ins = lir->insLoadi(obj_ins, offsetof(JSObject, dslots));
    return lir->insLoadi(dslots_ins, (slot - JS_INITIAL_NSLOTS) * sizeof(jsval));
}

bool
TraceRecorder::native_set(LIns* obj_ins, JSScopeProperty* sprop, LIns*& dslots_ins, LIns* v_ins)
{
    if (SPROP_HAS_STUB_SETTER(sprop) && sprop->slot != SPROP_INVALID_SLOT) {
        stobj_set_slot(obj_ins, sprop->slot, dslots_ins, v_ins);
        return true;
    }
    return false;
}

bool
TraceRecorder::native_get(LIns* obj_ins, LIns* pobj_ins, JSScopeProperty* sprop,
        LIns*& dslots_ins, LIns*& v_ins)
{
    if (!SPROP_HAS_STUB_GETTER(sprop))
        return false;

    if (sprop->slot != SPROP_INVALID_SLOT)
        v_ins = stobj_get_slot(pobj_ins, sprop->slot, dslots_ins);
    else
        v_ins = lir->insImm(JSVAL_VOID);
    return true;
}

bool
TraceRecorder::box_jsval(jsval v, LIns*& v_ins)
{
    if (isNumber(v)) {
        LIns* args[] = { v_ins, cx_ins };
        v_ins = lir->insCall(F_BoxDouble, args);
        guard(false, lir->ins2(LIR_eq, v_ins, lir->insImmPtr((void*)JSVAL_ERROR_COOKIE)));
        return true;
    }
    switch (JSVAL_TAG(v)) {
      case JSVAL_BOOLEAN:
        v_ins = lir->ins2i(LIR_or, lir->ins2i(LIR_lsh, v_ins, JSVAL_TAGBITS), JSVAL_BOOLEAN);
        return true;
    }
    return false;
}

bool
TraceRecorder::unbox_jsval(jsval v, LIns*& v_ins)
{
    if (isNumber(v)) {
        // JSVAL_IS_NUMBER(v)
        guard(true, lir->ins_eq0(
                lir->ins_eq0(
                        lir->ins2(LIR_and, v_ins,
                                lir->insImmPtr((void*)(JSVAL_INT | JSVAL_DOUBLE))))));
        v_ins = lir->insCall(F_UnboxDouble, &v_ins);
        return true;
    }
    switch (JSVAL_TAG(v)) {
      case JSVAL_BOOLEAN:
        guard(true,
              lir->ins2i(LIR_eq,
                         lir->ins2(LIR_and, v_ins, lir->insImmPtr((void*)~JSVAL_TRUE)),
                         JSVAL_BOOLEAN));
         v_ins = lir->ins2i(LIR_ush, v_ins, JSVAL_TAGBITS);
         return true;
       case JSVAL_OBJECT:
        guard(true,
              lir->ins2i(LIR_eq,
                         lir->ins2(LIR_and, v_ins, lir->insImmPtr((void*)~JSVAL_TRUE)),
                         JSVAL_OBJECT));
        return true;
               
    }
    return false;
}

bool
TraceRecorder::guardThatObjectHasClass(JSObject* obj, LIns* obj_ins,
                                       JSClass* cls, LIns*& dslots_ins)
{
    if (STOBJ_GET_CLASS(obj) != cls)
        return false;
    LIns* class_ins = stobj_get_slot(obj_ins, JSSLOT_CLASS, dslots_ins);
    class_ins = lir->ins2(LIR_and, class_ins, lir->insImmPtr((void*)~3));
    guard(true, lir->ins2(LIR_eq, class_ins, lir->insImmPtr(cls)));
    return true;
}

bool TraceRecorder::guardThatObjectIsDenseArray(JSObject* obj, LIns* obj_ins, LIns*& dslots_ins)
{
    return guardThatObjectHasClass(obj, obj_ins, &js_ArrayClass, dslots_ins);
}

bool TraceRecorder::guardDenseArrayIndexWithinBounds(JSObject* obj, jsint idx,
        LIns* obj_ins, LIns*& dslots_ins, LIns* idx_ins)
{
    jsuint length = ARRAY_DENSE_LENGTH(obj);
    if (!((jsuint)idx < length && idx < obj->fslots[JSSLOT_ARRAY_LENGTH]))
        return false;
    if (!dslots_ins)
        dslots_ins = lir->insLoadi(obj_ins, offsetof(JSObject, dslots));
    LIns* length_ins = stobj_get_slot(obj_ins, JSSLOT_ARRAY_LENGTH, dslots_ins);
    // guard(index >= 0)
    guard(true, lir->ins2i(LIR_ge, idx_ins, 0));
    // guard(index < length)
    guard(true, lir->ins2(LIR_lt, idx_ins, length_ins));
    // guard(index < capacity)
    guard(false, lir->ins_eq0(dslots_ins));
    guard(true, lir->ins2(LIR_lt, idx_ins,
                          lir->insLoadi(dslots_ins, -sizeof(jsval))));
    return true;
}

bool TraceRecorder::record_JSOP_INTERRUPT()
{
    return false;
}
bool TraceRecorder::record_JSOP_PUSH()
{
    stack(0, lir->insImm(JSVAL_VOID));
    return true;
}
bool TraceRecorder::record_JSOP_POPV()
{
    jsval& v = stackval(-1);
    set(&cx->fp->rval, get(&v));
    return true;
}
bool TraceRecorder::record_JSOP_ENTERWITH()
{
    return false;
}
bool TraceRecorder::record_JSOP_LEAVEWITH()
{
    return false;
}
bool TraceRecorder::record_JSOP_RETURN()
{
    return false;
}
bool TraceRecorder::record_JSOP_GOTO()
{
    return true;
}
bool TraceRecorder::record_JSOP_IFEQ()
{
    return ifop();
}
bool TraceRecorder::record_JSOP_IFNE()
{
    return ifop();
}
bool TraceRecorder::record_JSOP_ARGUMENTS()
{
    return false;
}
bool TraceRecorder::record_JSOP_FORARG()
{
    return false;
}
bool TraceRecorder::record_JSOP_FORVAR()
{
    return false;
}
bool TraceRecorder::record_JSOP_DUP()
{
    stack(0, get(&stackval(-1)));
    return true;
}
bool TraceRecorder::record_JSOP_DUP2()
{
    stack(0, get(&stackval(-2)));
    stack(1, get(&stackval(-1)));
    return true;
}
bool TraceRecorder::record_JSOP_SETCONST()
{
    return false;
}
bool TraceRecorder::record_JSOP_BITOR()
{
    return binary(LIR_or);
}
bool TraceRecorder::record_JSOP_BITXOR()
{
    return binary(LIR_xor);
}
bool TraceRecorder::record_JSOP_BITAND()
{
    return binary(LIR_and);
}
bool TraceRecorder::record_JSOP_EQ()
{
    return cmp(LIR_feq);
}
bool TraceRecorder::record_JSOP_NE()
{
    return cmp(LIR_feq, true);
}
bool TraceRecorder::record_JSOP_LT()
{
    return cmp(LIR_flt);
}
bool TraceRecorder::record_JSOP_LE()
{
    return cmp(LIR_fle);
}
bool TraceRecorder::record_JSOP_GT()
{
    return cmp(LIR_fgt);
}
bool TraceRecorder::record_JSOP_GE()
{
    return cmp(LIR_fge);
}
bool TraceRecorder::record_JSOP_LSH()
{
    return binary(LIR_lsh);
}
bool TraceRecorder::record_JSOP_RSH()
{
    return binary(LIR_rsh);
}
bool TraceRecorder::record_JSOP_URSH()
{
    return binary(LIR_ush);
}
bool TraceRecorder::record_JSOP_ADD()
{
    return binary(LIR_fadd);
}
bool TraceRecorder::record_JSOP_SUB()
{
    return binary(LIR_fsub);
}
bool TraceRecorder::record_JSOP_MUL()
{
    return binary(LIR_fmul);
}
bool TraceRecorder::record_JSOP_DIV()
{
    return binary(LIR_fdiv);
}
bool TraceRecorder::record_JSOP_MOD()
{
    return false;
}
bool TraceRecorder::record_JSOP_NOT()
{
    jsval& v = stackval(-1);
    if (JSVAL_IS_BOOLEAN(v)) {
        set(&v, lir->ins_eq0(get(&v)));
        return true;
    }
    return false;
}
bool TraceRecorder::record_JSOP_BITNOT()
{
    return unary(LIR_not);
}
bool TraceRecorder::record_JSOP_NEG()
{
    return unary(LIR_fneg);
}
bool TraceRecorder::record_JSOP_NEW()
{
    return false;
}
bool TraceRecorder::record_JSOP_DELNAME()
{
    return false;
}
bool TraceRecorder::record_JSOP_DELPROP()
{
    return false;
}
bool TraceRecorder::record_JSOP_DELELEM()
{
    return false;
}
bool TraceRecorder::record_JSOP_TYPEOF()
{
    return false;
}
bool TraceRecorder::record_JSOP_VOID()
{
    return false;
}
bool TraceRecorder::record_JSOP_INCNAME()
{
    return false;
}
bool TraceRecorder::record_JSOP_INCPROP()
{
    return false;
}
bool TraceRecorder::record_JSOP_INCELEM()
{
    return false;
}
bool TraceRecorder::record_JSOP_DECNAME()
{
    return false;
}

bool TraceRecorder::record_JSOP_DECPROP()
{
    return false;
}

bool TraceRecorder::record_JSOP_DECELEM()
{
    return false;
}

bool TraceRecorder::record_JSOP_NAMEINC()
{
    return false;
}

bool TraceRecorder::record_JSOP_PROPINC()
{
    return false;
}

bool TraceRecorder::record_JSOP_ELEMINC()
{
    return false;
}

bool TraceRecorder::record_JSOP_NAMEDEC()
{
    return false;
}

bool TraceRecorder::record_JSOP_PROPDEC()
{
    return false;
}

bool TraceRecorder::record_JSOP_ELEMDEC()
{
    return false;
}

bool TraceRecorder::record_JSOP_GETPROP()
{
    return false;
}

bool TraceRecorder::record_JSOP_SETPROP()
{
    return false;
}

bool TraceRecorder::record_JSOP_GETELEM()
{
    jsval& r = stackval(-1);
    jsval& l = stackval(-2);
    /* no guards for type checks, trace specialized this already */
    if (!JSVAL_IS_INT(r) || JSVAL_IS_PRIMITIVE(l))
        return false;
    JSObject* obj = JSVAL_TO_OBJECT(l);
    LIns* obj_ins = get(&l);
    /* make sure the object is actually a dense array */
    LIns* dslots_ins = lir->insLoadi(obj_ins, offsetof(JSObject, dslots));
    if (!guardThatObjectIsDenseArray(obj, obj_ins, dslots_ins))
        return false;
    /* check that the index is within bounds */
    jsint idx = JSVAL_TO_INT(r);
    LIns* idx_ins = f2i(get(&r));
    /* we have to check that its really an integer, but this check will to go away
       once we peel the loop type down to integer for this slot */
    guard(true, lir->ins2(LIR_feq, get(&r), lir->ins1(LIR_i2f, idx_ins)));
    if (!guardDenseArrayIndexWithinBounds(obj, idx, obj_ins, dslots_ins, idx_ins))
        return false;
    jsval v = obj->dslots[idx];
    /* load the value, check the type (need to check JSVAL_HOLE only for booleans) */
    LIns* v_ins = lir->insLoad(LIR_ld, 
            lir->ins2(LIR_add, dslots_ins, 
                        lir->ins2i(LIR_lsh, idx_ins, sizeof(jsval) == 4 ? 2 : 3)), 0);
    if (!unbox_jsval(v, v_ins))
        return false;
    set(&l, v_ins);
    return true;
}

bool TraceRecorder::record_JSOP_SETELEM()
{
    jsval& v = stackval(-1);
    jsval& r = stackval(-2);
    jsval& l = stackval(-3);
    /* no guards for type checks, trace specialized this already */
    if (!JSVAL_IS_INT(r) || JSVAL_IS_PRIMITIVE(l))
        return false;
    JSObject* obj = JSVAL_TO_OBJECT(l);
    LIns* obj_ins = get(&l);
    /* make sure the object is actually a dense array */
    LIns* dslots_ins = lir->insLoadi(obj_ins, offsetof(JSObject, dslots));
    if (!guardThatObjectIsDenseArray(obj, obj_ins, dslots_ins))
        return false;
    /* check that the index is within bounds */
    jsint idx = JSVAL_TO_INT(r);
    LIns* idx_ins = f2i(get(&r));
    /* we have to check that its really an integer, but this check will to go away
       once we peel the loop type down to integer for this slot */
    guard(true, lir->ins2(LIR_feq, get(&r), lir->ins1(LIR_i2f, idx_ins)));
    if (!guardDenseArrayIndexWithinBounds(obj, idx, obj_ins, dslots_ins, idx_ins))
        return false;
    /* get us the address of the array slot */
    LIns* addr = lir->ins2(LIR_add, dslots_ins,
                           lir->ins2i(LIR_lsh, idx_ins, JS_BYTES_PER_WORD_LOG2));
    LIns* oldval = lir->insLoad(LIR_ld, addr, 0);
    LIns* isHole = lir->ins2(LIR_eq, oldval, lir->insImmPtr((void*)JSVAL_HOLE));
    LIns* count = lir->insLoadi(obj_ins,
                                offsetof(JSObject, fslots[JSSLOT_ARRAY_COUNT]));
    lir->insStorei(lir->ins2(LIR_add, count, isHole), obj_ins,
                   offsetof(JSObject, fslots[JSSLOT_ARRAY_COUNT]));
    /* ok, box the value we are storing, store it and we are done */
    LIns* v_ins = get(&v);
    LIns* boxed_ins = v_ins;
    if (!box_jsval(v, boxed_ins))
        return false;
    lir->insStorei(boxed_ins, addr, 0);
    set(&l, v_ins);
    return true;
}

bool TraceRecorder::record_JSOP_CALLNAME()
{
    return false;
}

JSBool
js_math_sin(JSContext *cx, uintN argc, jsval *vp);

JSBool
js_math_cos(JSContext *cx, uintN argc, jsval *vp);

JSBool
js_math_pow(JSContext *cx, uintN argc, jsval *vp);

bool TraceRecorder::record_JSOP_CALL()
{
    uintN argc = GET_ARGC(cx->fp->regs->pc);
    jsval& fval = stackval(-(argc + 2));

    if (!VALUE_IS_FUNCTION(cx, fval))
        ABORT_TRACE("CALL on non-function");

    JSFunction* fun = GET_FUNCTION_PRIVATE(cx, JSVAL_TO_OBJECT(fval));
    if (FUN_INTERPRETED(fun))
        ABORT_TRACE("scripted function");

    JSFastNative native = (JSFastNative)fun->u.n.native;
    LIns* result;
    if (native == js_math_sin || native == js_math_cos) {
        if (argc != 1)
            ABORT_TRACE("Math.sin/cos: need exactly one arg");
        
        jsval& arg = stackval(-1);
        if (!isNumber(arg))
            ABORT_TRACE("Math.sin/cos: only numeric arg permitted");
        
        LIns* arg_ins = get(&arg);
        result = lir->insCall(native == js_math_sin ? F_Math_dot_sin : F_Math_dot_cos, &arg_ins);
    } else if (native == js_math_pow) {
        if (argc != 2)
            ABORT_TRACE("Math.pow: need exactly two args");
        
        jsval& arg1 = stackval(-2);
        jsval& arg2 = stackval(-1);
        
        if (!isNumber(arg1) || !isNumber(arg2))
            ABORT_TRACE("Math.pow: both args must be numeric");
        
        LIns* args[] = { get(&arg2), get(&arg1) };
        result = lir->insCall(F_Math_dot_pow, args);
    } else {
        ABORT_TRACE("only Math.{sin,cos,pow}");
    }
    set(&fval, result);
    return true;
}

bool TraceRecorder::record_JSOP_NAME()
{
    JSObject* obj = cx->fp->scopeChain;
    if (obj != global->varobj)
        return false;

    LIns* obj_ins = lir->insLoadi(lir->insLoadi(cx_ins, offsetof(JSContext, fp)),
                                  offsetof(JSStackFrame, scopeChain));
    uint32 slot;
    if (!test_property_cache_direct_slot(obj, obj_ins, slot))
        return false;

    stack(0, get(&STOBJ_GET_SLOT(obj, slot)));
    return true;
}

bool TraceRecorder::record_JSOP_DOUBLE()
{
    jsval v = jsval(atoms[GET_INDEX(cx->fp->regs->pc)]);
    stack(0, lir->insImmq(*(uint64_t*)JSVAL_TO_DOUBLE(v)));
    return true;
}
bool TraceRecorder::record_JSOP_STRING()
{
    return false;
}
bool TraceRecorder::record_JSOP_ZERO()
{
    jsdouble d = (jsdouble)0;
    stack(0, lir->insImmq(*(uint64_t*)&d));
    return true;
}
bool TraceRecorder::record_JSOP_ONE()
{
    jsdouble d = (jsdouble)1;
    stack(0, lir->insImmq(*(uint64_t*)&d));
    return true;
}
bool TraceRecorder::record_JSOP_NULL()
{
    stack(0, lir->insImmPtr(NULL));
    return true;
}
bool TraceRecorder::record_JSOP_THIS()
{
    return false;
}
bool TraceRecorder::record_JSOP_FALSE()
{
    stack(0, lir->insImm(0));
    return true;
}
bool TraceRecorder::record_JSOP_TRUE()
{
    stack(0, lir->insImm(1));
    return true;
}
bool TraceRecorder::record_JSOP_OR()
{
    return false;
}
bool TraceRecorder::record_JSOP_AND()
{
    return false;
}
bool TraceRecorder::record_JSOP_TABLESWITCH()
{
    return false;
}
bool TraceRecorder::record_JSOP_LOOKUPSWITCH()
{
    return false;
}
bool TraceRecorder::record_JSOP_STRICTEQ()
{
    return false;
}
bool TraceRecorder::record_JSOP_STRICTNE()
{
    return false;
}
bool TraceRecorder::record_JSOP_CLOSURE()
{
    return false;
}
bool TraceRecorder::record_JSOP_EXPORTALL()
{
    return false;
}
bool TraceRecorder::record_JSOP_EXPORTNAME()
{
    return false;
}
bool TraceRecorder::record_JSOP_IMPORTALL()
{
    return false;
}
bool TraceRecorder::record_JSOP_IMPORTPROP()
{
    return false;
}
bool TraceRecorder::record_JSOP_IMPORTELEM()
{
    return false;
}
bool TraceRecorder::record_JSOP_OBJECT()
{
    return false;
}
bool TraceRecorder::record_JSOP_POP()
{
    return true;
}
bool TraceRecorder::record_JSOP_POS()
{
    return false;
}
bool TraceRecorder::record_JSOP_TRAP()
{
    return false;
}
bool TraceRecorder::record_JSOP_GETARG()
{
    stack(0, arg(GET_ARGNO(cx->fp->regs->pc)));
    return true;
}
bool TraceRecorder::record_JSOP_SETARG()
{
    arg(GET_ARGNO(cx->fp->regs->pc), stack(-1));
    return true;
}
bool TraceRecorder::record_JSOP_GETVAR()
{
    stack(0, var(GET_VARNO(cx->fp->regs->pc)));
    return true;
}
bool TraceRecorder::record_JSOP_SETVAR()
{
    var(GET_VARNO(cx->fp->regs->pc), stack(-1));
    return true;
}
bool TraceRecorder::record_JSOP_UINT16()
{
    jsdouble d = (jsdouble)GET_UINT16(cx->fp->regs->pc);
    stack(0, lir->insImmq(*(uint64_t*)&d));
    return true;
}
bool TraceRecorder::record_JSOP_NEWINIT()
{
    return false;
}
bool TraceRecorder::record_JSOP_ENDINIT()
{
    return false;
}
bool TraceRecorder::record_JSOP_INITPROP()
{
    return false;
}
bool TraceRecorder::record_JSOP_INITELEM()
{
    return false;
}
bool TraceRecorder::record_JSOP_DEFSHARP()
{
    return false;
}
bool TraceRecorder::record_JSOP_USESHARP()
{
    return false;
}
bool TraceRecorder::record_JSOP_INCARG()
{
    return inc(argval(GET_ARGNO(cx->fp->regs->pc)), 1);
}
bool TraceRecorder::record_JSOP_INCVAR()
{
    return inc(varval(GET_VARNO(cx->fp->regs->pc)), 1);
}
bool TraceRecorder::record_JSOP_DECARG()
{
    return inc(argval(GET_ARGNO(cx->fp->regs->pc)), -1);
}
bool TraceRecorder::record_JSOP_DECVAR()
{
    return inc(varval(GET_VARNO(cx->fp->regs->pc)), -1);
}
bool TraceRecorder::record_JSOP_ARGINC()
{
    return inc(argval(GET_ARGNO(cx->fp->regs->pc)), 1, false);
}
bool TraceRecorder::record_JSOP_VARINC()
{
    return inc(varval(GET_VARNO(cx->fp->regs->pc)), 1, false);
}
bool TraceRecorder::record_JSOP_ARGDEC()
{
    return inc(argval(GET_ARGNO(cx->fp->regs->pc)), -1, false);
}
bool TraceRecorder::record_JSOP_VARDEC()
{
    return inc(varval(GET_VARNO(cx->fp->regs->pc)), -1, false);
}
bool TraceRecorder::record_JSOP_ITER()
{
    return false;
}
bool TraceRecorder::record_JSOP_FORNAME()
{
    return false;
}
bool TraceRecorder::record_JSOP_FORPROP()
{
    return false;
}
bool TraceRecorder::record_JSOP_FORELEM()
{
    return false;
}
bool TraceRecorder::record_JSOP_POPN()
{
    return true;
}
bool TraceRecorder::record_JSOP_BINDNAME()
{
    JSObject* obj = cx->fp->scopeChain;
    if (obj != global->varobj)
        return false;

    LIns* obj_ins = lir->insLoadi(lir->insLoadi(cx_ins, offsetof(JSContext, fp)),
                                  offsetof(JSStackFrame, scopeChain));
    JSObject* obj2;
    JSPropCacheEntry* entry;
    if (!test_property_cache(obj, obj_ins, obj2, entry))
        return false;

    stack(0, obj_ins);
    return true;
}

bool TraceRecorder::record_JSOP_SETNAME()
{
    jsval& r = stackval(-1);
    jsval& l = stackval(-2);

    if (JSVAL_IS_PRIMITIVE(l))
        return false;

    /*
     * Trace cases that are global code or in lightweight functions scoped by
     * the global object only.
     */
    JSObject* obj = JSVAL_TO_OBJECT(l);
    if (obj != cx->fp->scopeChain || obj != global->varobj)
        return false;

    LIns* obj_ins = get(&l);
    uint32 slot;
    if (!test_property_cache_direct_slot(obj, obj_ins, slot))
        return false;

    LIns* r_ins = get(&r);
    set(&STOBJ_GET_SLOT(obj, slot), r_ins);

    if (cx->fp->regs->pc[JSOP_SETNAME_LENGTH] != JSOP_POP)
        stack(-2, r_ins);
    return true;
}

bool TraceRecorder::record_JSOP_THROW()
{
    return false;
}
bool TraceRecorder::record_JSOP_IN()
{
    return false;
}
bool TraceRecorder::record_JSOP_INSTANCEOF()
{
    return false;
}
bool TraceRecorder::record_JSOP_DEBUGGER()
{
    return false;
}
bool TraceRecorder::record_JSOP_GOSUB()
{
    return false;
}
bool TraceRecorder::record_JSOP_RETSUB()
{
    return false;
}
bool TraceRecorder::record_JSOP_EXCEPTION()
{
    return false;
}
bool TraceRecorder::record_JSOP_LINENO()
{
    return true;
}
bool TraceRecorder::record_JSOP_CONDSWITCH()
{
    return true;
}
bool TraceRecorder::record_JSOP_CASE()
{
    return false;
}
bool TraceRecorder::record_JSOP_DEFAULT()
{
    return false;
}
bool TraceRecorder::record_JSOP_EVAL()
{
    return false;
}
bool TraceRecorder::record_JSOP_ENUMELEM()
{
    return false;
}
bool TraceRecorder::record_JSOP_GETTER()
{
    return false;
}
bool TraceRecorder::record_JSOP_SETTER()
{
    return false;
}
bool TraceRecorder::record_JSOP_DEFFUN()
{
    return false;
}
bool TraceRecorder::record_JSOP_DEFCONST()
{
    return false;
}
bool TraceRecorder::record_JSOP_DEFVAR()
{
    return false;
}
bool TraceRecorder::record_JSOP_ANONFUNOBJ()
{
    return false;
}
bool TraceRecorder::record_JSOP_NAMEDFUNOBJ()
{
    return false;
}
bool TraceRecorder::record_JSOP_SETLOCALPOP()
{
    return false;
}
bool TraceRecorder::record_JSOP_GROUP()
{
    return true; // no-op
}
bool TraceRecorder::record_JSOP_SETCALL()
{
    return false;
}
bool TraceRecorder::record_JSOP_TRY()
{
    return true;
}
bool TraceRecorder::record_JSOP_FINALLY()
{
    return true;
}
bool TraceRecorder::record_JSOP_NOP()
{
    return true;
}
bool TraceRecorder::record_JSOP_ARGSUB()
{
    return false;
}
bool TraceRecorder::record_JSOP_ARGCNT()
{
    return false;
}
bool TraceRecorder::record_JSOP_DEFLOCALFUN()
{
    return false;
}
bool TraceRecorder::record_JSOP_GOTOX()
{
    return false;
}
bool TraceRecorder::record_JSOP_IFEQX()
{
    return record_JSOP_IFEQ();
}
bool TraceRecorder::record_JSOP_IFNEX()
{
    return record_JSOP_IFNE();
}
bool TraceRecorder::record_JSOP_ORX()
{
    return record_JSOP_OR();
}
bool TraceRecorder::record_JSOP_ANDX()
{
    return record_JSOP_AND();
}
bool TraceRecorder::record_JSOP_GOSUBX()
{
    return record_JSOP_GOSUB();
}
bool TraceRecorder::record_JSOP_CASEX()
{
    return record_JSOP_CASE();
}
bool TraceRecorder::record_JSOP_DEFAULTX()
{
    return record_JSOP_DEFAULT();
}
bool TraceRecorder::record_JSOP_TABLESWITCHX()
{
    return record_JSOP_TABLESWITCH();
}
bool TraceRecorder::record_JSOP_LOOKUPSWITCHX()
{
    return record_JSOP_LOOKUPSWITCH();
}
bool TraceRecorder::record_JSOP_BACKPATCH()
{
    return true;
}
bool TraceRecorder::record_JSOP_BACKPATCH_POP()
{
    return true;
}
bool TraceRecorder::record_JSOP_THROWING()
{
    return false;
}
bool TraceRecorder::record_JSOP_SETRVAL()
{
    return false;
}
bool TraceRecorder::record_JSOP_RETRVAL()
{
    return false;
}

bool TraceRecorder::record_JSOP_GETGVAR()
{
    jsval slotval = cx->fp->vars[GET_VARNO(cx->fp->regs->pc)];
    if (JSVAL_IS_NULL(slotval))
        return true; // We will see JSOP_NAME from the interpreter's jump, so no-op here.

    uint32 slot = JSVAL_TO_INT(slotval);
    stack(0, get(&STOBJ_GET_SLOT(cx->fp->scopeChain, slot)));
    return true;
}

bool TraceRecorder::record_JSOP_SETGVAR()
{
    jsval slotval = cx->fp->vars[GET_VARNO(cx->fp->regs->pc)];
    if (JSVAL_IS_NULL(slotval))
        return true; // We will see JSOP_NAME from the interpreter's jump, so no-op here.

    uint32 slot = JSVAL_TO_INT(slotval);
    set(&STOBJ_GET_SLOT(cx->fp->scopeChain, slot), stack(-1));
    return true;
}

bool TraceRecorder::record_JSOP_INCGVAR()
{
    jsval slotval = cx->fp->vars[GET_VARNO(cx->fp->regs->pc)];
    if (JSVAL_IS_NULL(slotval))
        return true; // We will see JSOP_INCNAME from the interpreter's jump, so no-op here.

    uint32 slot = JSVAL_TO_INT(slotval);
    return inc(STOBJ_GET_SLOT(cx->fp->scopeChain, slot), 1);
}

bool TraceRecorder::record_JSOP_DECGVAR()
{
    jsval slotval = cx->fp->vars[GET_VARNO(cx->fp->regs->pc)];
    if (JSVAL_IS_NULL(slotval))
        return true; // We will see JSOP_INCNAME from the interpreter's jump, so no-op here.

    uint32 slot = JSVAL_TO_INT(slotval);
    return inc(STOBJ_GET_SLOT(cx->fp->scopeChain, slot), -1);
}

bool TraceRecorder::record_JSOP_GVARINC()
{
    jsval slotval = cx->fp->vars[GET_VARNO(cx->fp->regs->pc)];
    if (JSVAL_IS_NULL(slotval))
        return true; // We will see JSOP_INCNAME from the interpreter's jump, so no-op here.

    uint32 slot = JSVAL_TO_INT(slotval);
    return inc(STOBJ_GET_SLOT(cx->fp->scopeChain, slot), 1, false);
}

bool TraceRecorder::record_JSOP_GVARDEC()
{
    jsval slotval = cx->fp->vars[GET_VARNO(cx->fp->regs->pc)];
    if (JSVAL_IS_NULL(slotval))
        return true; // We will see JSOP_INCNAME from the interpreter's jump, so no-op here.

    uint32 slot = JSVAL_TO_INT(slotval);
    return inc(STOBJ_GET_SLOT(cx->fp->scopeChain, slot), -1, false);
}

bool TraceRecorder::record_JSOP_REGEXP()
{
    return false;
}
bool TraceRecorder::record_JSOP_DEFXMLNS()
{
    return false;
}
bool TraceRecorder::record_JSOP_ANYNAME()
{
    return false;
}
bool TraceRecorder::record_JSOP_QNAMEPART()
{
    return false;
}
bool TraceRecorder::record_JSOP_QNAMECONST()
{
    return false;
}
bool TraceRecorder::record_JSOP_QNAME()
{
    return false;
}
bool TraceRecorder::record_JSOP_TOATTRNAME()
{
    return false;
}
bool TraceRecorder::record_JSOP_TOATTRVAL()
{
    return false;
}
bool TraceRecorder::record_JSOP_ADDATTRNAME()
{
    return false;
}
bool TraceRecorder::record_JSOP_ADDATTRVAL()
{
    return false;
}
bool TraceRecorder::record_JSOP_BINDXMLNAME()
{
    return false;
}
bool TraceRecorder::record_JSOP_SETXMLNAME()
{
    return false;
}
bool TraceRecorder::record_JSOP_XMLNAME()
{
    return false;
}
bool TraceRecorder::record_JSOP_DESCENDANTS()
{
    return false;
}
bool TraceRecorder::record_JSOP_FILTER()
{
    return false;
}
bool TraceRecorder::record_JSOP_ENDFILTER()
{
    return false;
}
bool TraceRecorder::record_JSOP_TOXML()
{
    return false;
}
bool TraceRecorder::record_JSOP_TOXMLLIST()
{
    return false;
}
bool TraceRecorder::record_JSOP_XMLTAGEXPR()
{
    return false;
}
bool TraceRecorder::record_JSOP_XMLELTEXPR()
{
    return false;
}
bool TraceRecorder::record_JSOP_XMLOBJECT()
{
    return false;
}
bool TraceRecorder::record_JSOP_XMLCDATA()
{
    return false;
}
bool TraceRecorder::record_JSOP_XMLCOMMENT()
{
    return false;
}
bool TraceRecorder::record_JSOP_XMLPI()
{
    return false;
}

bool TraceRecorder::record_JSOP_CALLPROP()
{
    jsval& l = stackval(-1);
    if (JSVAL_IS_PRIMITIVE(l))
        ABORT_TRACE("CALLPROP on primitive");

    JSObject* obj = JSVAL_TO_OBJECT(l);
    LIns* obj_ins = get(&l);
    JSObject* obj2;
    JSPropCacheEntry* entry;
    if (!test_property_cache(obj, obj_ins, obj2, entry))
        ABORT_TRACE("missed prop");

    if (!PCVAL_IS_OBJECT(entry->vword))
        ABORT_TRACE("PCE not object");

    stack(-1, lir->insImmPtr(PCVAL_TO_OBJECT(entry->vword)));
    stack(0, obj_ins);
    return true;
}

bool TraceRecorder::record_JSOP_GETFUNNS()
{
    return false;
}
bool TraceRecorder::record_JSOP_UNUSED186()
{
    return false;
}
bool TraceRecorder::record_JSOP_DELDESC()
{
    return false;
}
bool TraceRecorder::record_JSOP_UINT24()
{
    jsdouble d = (jsdouble) GET_UINT24(cx->fp->regs->pc);
    stack(0, lir->insImmq(*(uint64_t*)&d));
    return true;
}
bool TraceRecorder::record_JSOP_INDEXBASE()
{
    atoms += GET_INDEXBASE(cx->fp->regs->pc);
    return true;
}

bool TraceRecorder::record_JSOP_RESETBASE()
{
    atoms = cx->fp->script->atomMap.vector;
    return true;
}

bool TraceRecorder::record_JSOP_RESETBASE0()
{
    atoms = cx->fp->script->atomMap.vector;
    return true;
}

bool TraceRecorder::record_JSOP_STARTXML()
{
    return false;
}

bool TraceRecorder::record_JSOP_STARTXMLEXPR()
{
    return false;
}

bool TraceRecorder::record_JSOP_CALLELEM()
{
    return false;
}

bool TraceRecorder::record_JSOP_STOP()
{
    return true;
}

bool TraceRecorder::record_JSOP_GETXPROP()
{
    return false;
}

bool TraceRecorder::record_JSOP_CALLXMLNAME()
{
    return false;
}

bool TraceRecorder::record_JSOP_TYPEOFEXPR()
{
    return false;
}

bool TraceRecorder::record_JSOP_ENTERBLOCK()
{
    return false;
}

bool TraceRecorder::record_JSOP_LEAVEBLOCK()
{
    return false;
}

bool TraceRecorder::record_JSOP_GETLOCAL()
{
    return false;
}

bool TraceRecorder::record_JSOP_SETLOCAL()
{
    return false;
}

bool TraceRecorder::record_JSOP_INCLOCAL()
{
    return false;
}

bool TraceRecorder::record_JSOP_DECLOCAL()
{
    return false;
}

bool TraceRecorder::record_JSOP_LOCALINC()
{
    return false;
}

bool TraceRecorder::record_JSOP_LOCALDEC()
{
    return false;
}

bool TraceRecorder::record_JSOP_FORLOCAL()
{
    return false;
}

bool TraceRecorder::record_JSOP_FORCONST()
{
    return false;
}

bool TraceRecorder::record_JSOP_ENDITER()
{
    return false;
}

bool TraceRecorder::record_JSOP_GENERATOR()
{
    return false;
}

bool TraceRecorder::record_JSOP_YIELD()
{
    return false;
}

bool TraceRecorder::record_JSOP_ARRAYPUSH()
{
    return false;
}

bool TraceRecorder::record_JSOP_UNUSED213()
{
    return false;
}

bool TraceRecorder::record_JSOP_ENUMCONSTELEM()
{
    return false;
}

bool TraceRecorder::record_JSOP_LEAVEBLOCKEXPR()
{
    return false;
}

bool TraceRecorder::record_JSOP_GETTHISPROP()
{
    return false;
}

bool TraceRecorder::record_JSOP_GETARGPROP()
{
    return false;
}

bool TraceRecorder::record_JSOP_GETVARPROP()
{
    return false;
}

bool TraceRecorder::record_JSOP_GETLOCALPROP()
{
    return false;
}

bool TraceRecorder::record_JSOP_INDEXBASE1()
{
    atoms += 1 << 16;
    return true;
}

bool TraceRecorder::record_JSOP_INDEXBASE2()
{
    atoms += 2 << 16;
    return true;
}

bool TraceRecorder::record_JSOP_INDEXBASE3()
{
    atoms += 3 << 16;
    return true;
}

bool TraceRecorder::record_JSOP_CALLGVAR()
{
    return false;
}

bool TraceRecorder::record_JSOP_CALLVAR()
{
    return false;
}

bool TraceRecorder::record_JSOP_CALLARG()
{
    return false;
}

bool TraceRecorder::record_JSOP_CALLLOCAL()
{
    return false;
}

bool TraceRecorder::record_JSOP_INT8()
{
    jsdouble d = (jsdouble)GET_INT8(cx->fp->regs->pc);
    stack(0, lir->insImmq(*(uint64_t*)&d));
    return true;
}

bool TraceRecorder::record_JSOP_INT32()
{
    jsdouble d = (jsdouble)GET_INT32(cx->fp->regs->pc);
    stack(0, lir->insImmq(*(uint64_t*)&d));
    return true;
}

bool TraceRecorder::record_JSOP_LENGTH()
{
    return false;
}

bool TraceRecorder::record_JSOP_NEWARRAY()
{
    return false;
}

bool TraceRecorder::record_JSOP_HOLE()
{
    stack(0, lir->insImm(JSVAL_HOLE));
    return true;
}
