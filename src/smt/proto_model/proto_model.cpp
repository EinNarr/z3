/*++
Copyright (c) 2006 Microsoft Corporation

Module Name:

    proto_model.cpp

Abstract:

    <abstract>

Author:

    Leonardo de Moura (leonardo) 2007-03-08.

Revision History:

--*/
#include"proto_model.h"
#include"model_params.hpp"
#include"ast_pp.h"
#include"ast_ll_pp.h"
#include"var_subst.h"
#include"array_decl_plugin.h"
#include"well_sorted.h"
#include"used_symbols.h"
#include"model_v2_pp.h"

proto_model::proto_model(ast_manager & m, params_ref const & p):
    model_core(m),
    m_afid(m.mk_family_id(symbol("array"))),
    m_eval(*this),
    m_rewrite(m) {
    register_factory(alloc(basic_factory, m));
    m_user_sort_factory = alloc(user_sort_factory, m);
    register_factory(m_user_sort_factory);
    
    m_model_partial = model_params(p).partial();
}



void proto_model::register_aux_decl(func_decl * d, func_interp * fi) {
    model_core::register_decl(d, fi);
    m_aux_decls.insert(d);
}

/**
   \brief Set new_fi as the new interpretation for f.
   If f_aux != 0, then assign the old interpretation of f to f_aux.
   If f_aux == 0, then delete the old interpretation of f.

   f_aux is marked as a auxiliary declaration.
*/
void proto_model::reregister_decl(func_decl * f, func_interp * new_fi, func_decl * f_aux) {
    func_interp * fi = get_func_interp(f);
    if (fi == 0) {
        register_decl(f, new_fi);
    }
    else {
        if (f_aux != 0) {
            register_decl(f_aux, fi);
            m_aux_decls.insert(f_aux);
        }
        else {
            dealloc(fi);
        }
        m_finterp.insert(f, new_fi);
    }
}

expr * proto_model::mk_some_interp_for(func_decl * d) {
    SASSERT(!has_interpretation(d));
    expr * r = get_some_value(d->get_range()); // if t is a function, then it will be the constant function.
    if (d->get_arity() == 0) {
        register_decl(d, r);
    }
    else {
        func_interp * new_fi = alloc(func_interp, m_manager, d->get_arity());
        new_fi->set_else(r);
        register_decl(d, new_fi);
    }
    return r;
}


bool proto_model::is_select_of_model_value(expr* e) const {
    return 
        is_app_of(e, m_afid, OP_SELECT) && 
        is_as_array(to_app(e)->get_arg(0)) &&
        has_interpretation(array_util(m_manager).get_as_array_func_decl(to_app(to_app(e)->get_arg(0))));
}

/**
   \brief Evaluate the expression e in the current model, and store the result in \c result.
   It returns \c true if succeeded, and false otherwise. If the evaluation fails,
   then r contains a term that is simplified as much as possible using the interpretations
   available in the model.
   
   When model_completion == true, if the model does not assign an interpretation to a
   declaration it will build one for it. Moreover, partial functions will also be completed.
   So, if model_completion == true, the evaluator never fails if it doesn't contain quantifiers.
*/
bool proto_model::eval(expr * e, expr_ref & result, bool model_completion) {
    m_eval.set_model_completion(model_completion);
    try {
        m_eval(e, result);
        return true;
    }
    catch (model_evaluator_exception & ex) {
        (void)ex;
        TRACE("model_evaluator", tout << ex.msg() << "\n";);
        return false;
    }
}

/**
   \brief Replace uninterpreted constants occurring in fi->get_else()
   by their interpretations.
*/
void proto_model::cleanup_func_interp(func_interp * fi, func_decl_set & found_aux_fs) {
    if (fi->is_partial())
        return;
    expr * fi_else = fi->get_else();
    TRACE("model_bug", tout << "cleaning up:\n" << mk_pp(fi_else, m_manager) << "\n";);

    obj_map<expr, expr*> cache;
    expr_ref_vector trail(m_manager);
    ptr_buffer<expr, 128> todo;
    ptr_buffer<expr> args;
    todo.push_back(fi_else);

    expr * a;
    while (!todo.empty()) {
        a = todo.back();
        if (is_uninterp_const(a)) {
            todo.pop_back();
            func_decl * a_decl = to_app(a)->get_decl();
            expr * ai = get_const_interp(a_decl);
            if (ai == 0) {
                ai = get_some_value(a_decl->get_range());
                register_decl(a_decl, ai);
            }
            cache.insert(a, ai);
        }
        else {
            switch(a->get_kind()) {
            case AST_APP: {
                app * t = to_app(a);
                bool visited = true;
                args.reset();
                unsigned num_args = t->get_num_args();
                for (unsigned i = 0; i < num_args; ++i) {
                    expr * arg = 0;
                    if (!cache.find(t->get_arg(i), arg)) {
                        visited = false;
                        todo.push_back(t->get_arg(i));
                    }
                    else {
                        args.push_back(arg);
                    }
                }
                if (!visited) {
                    continue;
                }
                func_decl * f = t->get_decl();
                if (m_aux_decls.contains(f))
                    found_aux_fs.insert(f);
                expr_ref new_t(m_manager);
                new_t = m_rewrite.mk_app(f, num_args, args.c_ptr());
                if (t != new_t.get())
                    trail.push_back(new_t);
                todo.pop_back();
                cache.insert(t, new_t);
                break;
            }
            default:
                SASSERT(a != 0);
                cache.insert(a, a);
                todo.pop_back();
                break;
            }
        }
    }

    if (!cache.find(fi_else, a)) {
        UNREACHABLE();
    }
    
    fi->set_else(a);
}

void proto_model::remove_aux_decls_not_in_set(ptr_vector<func_decl> & decls, func_decl_set const & s) {
    unsigned sz = decls.size();
    unsigned i  = 0;
    unsigned j  = 0;
    for (; i < sz; i++) {
        func_decl * f = decls[i];
        if (!m_aux_decls.contains(f) || s.contains(f)) {
            decls[j] = f;
            j++;
        }
    }
    decls.shrink(j);
}


/**
   \brief Replace uninterpreted constants occurring in the func_interp's get_else()
   by their interpretations.
*/
void proto_model::cleanup() {
    func_decl_set found_aux_fs;
    decl2finterp::iterator it  = m_finterp.begin();
    decl2finterp::iterator end = m_finterp.end();
    for (; it != end; ++it) {
        func_interp * fi = (*it).m_value;
        cleanup_func_interp(fi, found_aux_fs);
    }
    
    // remove auxiliary declarations that are not used.
    if (found_aux_fs.size() != m_aux_decls.size()) {
        remove_aux_decls_not_in_set(m_decls, found_aux_fs);
        remove_aux_decls_not_in_set(m_func_decls, found_aux_fs);
        
        func_decl_set::iterator it2  = m_aux_decls.begin();
        func_decl_set::iterator end2 = m_aux_decls.end();
        for (; it2 != end2; ++it2) {
            func_decl * faux = *it2;
            if (!found_aux_fs.contains(faux)) {
                TRACE("cleanup_bug", tout << "eliminating " << faux->get_name() << "\n";);
                func_interp * fi = 0;
                m_finterp.find(faux, fi);
                SASSERT(fi != 0);
                m_finterp.erase(faux);
                m_manager.dec_ref(faux);
                dealloc(fi);
            }
        }
        m_aux_decls.swap(found_aux_fs);
    }
}

value_factory * proto_model::get_factory(family_id fid) {
    return m_factories.get_plugin(fid);
}

void proto_model::freeze_universe(sort * s) {
    SASSERT(m_manager.is_uninterp(s));
    m_user_sort_factory->freeze_universe(s);
}

/**
   \brief Return the known universe of an uninterpreted sort.
*/
obj_hashtable<expr> const & proto_model::get_known_universe(sort * s) const {
    SASSERT(m_manager.is_uninterp(s));
    return m_user_sort_factory->get_known_universe(s);
}

ptr_vector<expr> const & proto_model::get_universe(sort * s) const {
    ptr_vector<expr> & tmp = const_cast<proto_model*>(this)->m_tmp;
    tmp.reset();
    obj_hashtable<expr> const & u = get_known_universe(s);
    obj_hashtable<expr>::iterator it = u.begin();
    obj_hashtable<expr>::iterator end = u.end();
    for (; it != end; ++it)
        tmp.push_back(*it);
    return tmp;
}

unsigned proto_model::get_num_uninterpreted_sorts() const { 
    return m_user_sort_factory->get_num_sorts(); 
}

sort * proto_model::get_uninterpreted_sort(unsigned idx) const { 
    SASSERT(idx < get_num_uninterpreted_sorts()); 
    return m_user_sort_factory->get_sort(idx); 
}

/**
   \brief Return true if the given sort is uninterpreted and has a finite interpretation
   in the model.
*/
bool proto_model::is_finite(sort * s) const {
    return m_manager.is_uninterp(s) && m_user_sort_factory->is_finite(s);
}

expr * proto_model::get_some_value(sort * s) {
    if (m_manager.is_uninterp(s)) {
        return m_user_sort_factory->get_some_value(s);
    }
    else {
        family_id fid = s->get_family_id();
        value_factory * f = get_factory(fid);
        if (f) 
            return f->get_some_value(s);
        // there is no factory for the family id, then assume s is uninterpreted.
        return m_user_sort_factory->get_some_value(s);
    }
}

bool proto_model::get_some_values(sort * s, expr_ref & v1, expr_ref & v2) {
    if (m_manager.is_uninterp(s)) {
        return m_user_sort_factory->get_some_values(s, v1, v2);
    }
    else {
        family_id fid = s->get_family_id();
        value_factory * f = get_factory(fid);
        if (f) 
            return f->get_some_values(s, v1, v2);
        else
            return false;
    }
}

expr * proto_model::get_fresh_value(sort * s) {
    if (m_manager.is_uninterp(s)) {
        return m_user_sort_factory->get_fresh_value(s);
    }
    else {
        family_id fid = s->get_family_id();    
        value_factory * f = get_factory(fid);
        if (f) 
            return f->get_fresh_value(s);
        else
            // Use user_sort_factory if the theory has no support for model construnction.
            // This is needed when dummy theories are used for arithmetic or arrays.
            return m_user_sort_factory->get_fresh_value(s);
    }
}

void proto_model::register_value(expr * n) {
    sort * s = m_manager.get_sort(n);
    if (m_manager.is_uninterp(s)) {
        m_user_sort_factory->register_value(n);
    }
    else {
        family_id fid = s->get_family_id();
        value_factory * f = get_factory(fid);
        if (f)
            f->register_value(n);
    }
}

bool proto_model::is_as_array(expr * v) const {
    return is_app_of(v, m_afid, OP_AS_ARRAY);
}

void proto_model::compress() {
    ptr_vector<func_decl>::iterator it  = m_func_decls.begin();
    ptr_vector<func_decl>::iterator end = m_func_decls.end();
    for (; it != end; ++it) {
        func_decl * f = *it;
        func_interp * fi = get_func_interp(f);
        SASSERT(fi != 0);
        fi->compress();
    }
}

/**
   \brief Complete the interpretation fi of f if it is partial.
   If f does not have an interpretation in the given model, then this is a noop.
*/
void proto_model::complete_partial_func(func_decl * f) {
    func_interp * fi = get_func_interp(f);
    if (fi && fi->is_partial()) {
        expr * else_value = 0;
#if 0 
        // For UFBV benchmarks, setting the "else" to false is not a good idea.
        // TODO: find a permanent solution. A possibility is to add another option.
        if (m_manager.is_bool(f->get_range())) {
            else_value = m_manager.mk_false();
        }
        else {
            else_value = fi->get_max_occ_result();
            if (else_value == 0)
                else_value = get_some_value(f->get_range());
        }
#else
        else_value = fi->get_max_occ_result();
        if (else_value == 0)
            else_value = get_some_value(f->get_range());
#endif
        fi->set_else(else_value);
    }
}

/**
   \brief Set the (else) field of function interpretations... 
*/
void proto_model::complete_partial_funcs() {
    if (m_model_partial)
        return;

    // m_func_decls may be "expanded" when we invoke get_some_value.
    // So, we must not use iterators to traverse it.
    for (unsigned i = 0; i < m_func_decls.size(); i++) {
        complete_partial_func(m_func_decls[i]);
    }
}

model * proto_model::mk_model() {
    TRACE("proto_model", tout << "mk_model\n"; model_v2_pp(tout, *this););
    model * m = alloc(model, m_manager);

    decl2expr::iterator it1  = m_interp.begin();
    decl2expr::iterator end1 = m_interp.end();
    for (; it1 != end1; ++it1) {
        m->register_decl(it1->m_key, it1->m_value);
    }

    decl2finterp::iterator it2  = m_finterp.begin();
    decl2finterp::iterator end2 = m_finterp.end();
    for (; it2 != end2; ++it2) {
        m->register_decl(it2->m_key, it2->m_value);
        m_manager.dec_ref(it2->m_key);
    }
    
    m_finterp.reset(); // m took the ownership of the func_interp's

    unsigned sz = get_num_uninterpreted_sorts();
    for (unsigned i = 0; i < sz; i++) {
        sort * s = get_uninterpreted_sort(i);
        TRACE("proto_model", tout << "copying uninterpreted sorts...\n" << mk_pp(s, m_manager) << "\n";);
        ptr_vector<expr> const& buf = get_universe(s);
        m->register_usort(s, buf.size(), buf.c_ptr());
    }

    return m;
}
