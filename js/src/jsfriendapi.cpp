/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
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
 * The Original Code is SpiderMonkey code.
 *
 * The Initial Developer of the Original Code is
 *   Mozilla Foundation
 * Portions created by the Initial Developer are Copyright (C) 2010
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
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

#include "jscntxt.h"
#include "jscompartment.h"
#include "jsfriendapi.h"

using namespace js;

JS_FRIEND_API(JSString *)
JS_GetAnonymousString(JSRuntime *rt)
{
    JS_ASSERT(rt->state == JSRTS_UP);
    return rt->atomState.anonymousAtom;
}

JS_FRIEND_API(JSObject *)
JS_FindCompilationScope(JSContext *cx, JSObject *obj)
{
    /*
     * We unwrap wrappers here. This is a little weird, but it's what's being
     * asked of us.
     */
    if (obj->isWrapper())
        obj = obj->unwrap();
    
    /*
     * Innerize the target_obj so that we compile in the correct (inner)
     * scope.
     */
    if (JSObjectOp op = obj->getClass()->ext.innerObject)
        obj = op(cx, obj);
    return obj;
}

JS_FRIEND_API(JSObject *)
JS_UnwrapObject(JSObject *obj)
{
    return obj->unwrap();
}

JS_FRIEND_API(JSObject *)
JS_GetFrameScopeChainRaw(JSStackFrame *fp)
{
    return &Valueify(fp)->scopeChain();
}
