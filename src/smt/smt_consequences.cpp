/*++
Copyright (c) 2006 Microsoft Corporation

Module Name:

    smt_consequences.cpp

Abstract:

    Tuned consequence finding for smt_context.

Author:

    nbjorner 2016-07-28.

Revision History:

--*/
#include "smt_context.h"
#include "ast_util.h"
#include "datatype_decl_plugin.h"

namespace smt {

    expr_ref context::antecedent2fml(uint_set const& vars) {
        expr_ref_vector premises(m_manager);
        uint_set::iterator it = vars.begin(), end = vars.end();
        for (; it != end; ++it) {
            expr* e =  bool_var2expr(*it);
            premises.push_back(get_assignment(*it) != l_false ? e : m_manager.mk_not(e));
        }
        return mk_and(premises);
    }

    void context::extract_fixed_consequences(literal lit, obj_map<expr, expr*>& vars, uint_set const& assumptions, expr_ref_vector& conseq) {
        ast_manager& m = m_manager;
        datatype_util dt(m);
        expr* e1, *e2;       
        expr_ref fml(m);        
        if (lit == true_literal) return;
        expr* e = bool_var2expr(lit.var());
        uint_set s;
        if (assumptions.contains(lit.var())) {
            s.insert(lit.var());
        }
        else {
            b_justification js = get_justification(lit.var());
            switch (js.get_kind()) {
            case b_justification::CLAUSE: {
                clause * cls = js.get_clause();
                unsigned num_lits = cls->get_num_literals();
                for (unsigned j = 0; j < num_lits; ++j) {
                    literal lit2 = cls->get_literal(j);
                    if (lit2.var() != lit.var()) {
                        s |= m_antecedents.find(lit2.var());
                    }
                }
                break;
            }
            case b_justification::BIN_CLAUSE: {
                s |= m_antecedents.find(js.get_literal().var());
                break;
            }
            case b_justification::AXIOM: {
                break;
            }
            case b_justification::JUSTIFICATION: {
                literal_vector literals;
                m_conflict_resolution->justification2literals(js.get_justification(), literals);
                for (unsigned j = 0; j < literals.size(); ++j) {
                    s |= m_antecedents.find(literals[j].var());
                }
                break;
            }
            }
        }
        m_antecedents.insert(lit.var(), s);
        TRACE("context", display_literal_verbose(tout, lit); tout << " " << s << "\n";);
        bool found = false;
        if (vars.contains(e)) {
            found = true;
            fml = lit.sign() ? m.mk_not(e) : e;
            vars.erase(e);
        }
        else if (!lit.sign() && m.is_eq(e, e1, e2)) {
            if (vars.contains(e2)) {
                std::swap(e1, e2);
            }
            if (vars.contains(e1) && m.is_value(e2)) {
                found = true;
                fml = e;
                vars.erase(e1);
            }
        }
        else if (!lit.sign() && is_app(e) && dt.is_recognizer(to_app(e)->get_decl())) {
            if (vars.contains(to_app(e)->get_arg(0))) {
                found = true;
                fml = m.mk_eq(to_app(e)->get_arg(0), m.mk_const(dt.get_recognizer_constructor(to_app(e)->get_decl())));
                vars.erase(to_app(e)->get_arg(0));
            }
        }
        if (found) {
            fml = m.mk_implies(antecedent2fml(s), fml);
            conseq.push_back(fml);
        }
    }

    void context::extract_fixed_consequences(unsigned start, obj_map<expr, expr*>& vars, uint_set const& assumptions, expr_ref_vector& conseq) {
        pop_to_search_lvl();
        SASSERT(!inconsistent());
        literal_vector const& lits = assigned_literals();
        unsigned sz = lits.size();
        for (unsigned i = start; i < sz; ++i) {            
            extract_fixed_consequences(lits[i], vars, assumptions, conseq);
        }
        SASSERT(!inconsistent());
    }

    unsigned context::delete_unfixed(obj_map<expr, expr*>& var2val, expr_ref_vector& unfixed) {
        ast_manager& m = m_manager;
        ptr_vector<expr> to_delete;
        obj_map<expr,expr*>::iterator it = var2val.begin(), end = var2val.end();
        for (; it != end; ++it) {
            expr* k = it->m_key;
            expr* v = it->m_value;
            if (m.is_bool(k)) {
                literal lit = get_literal(k);
                switch (get_assignment(lit)) {
                case l_true: 
                    if (m.is_false(v)) {
                        to_delete.push_back(k);
                    }
                    else {
                        force_phase(lit.var(), false);
                    }
                    break;
                case l_false:
                    if (m.is_true(v)) {
                        to_delete.push_back(k);
                    }
                    else {
                        force_phase(lit.var(), true);
                    }
                    break;
                default:
                    to_delete.push_back(k);
                    break;
                }
            }
            else if (e_internalized(k) && m.are_distinct(v, get_enode(k)->get_root()->get_owner())) {
                to_delete.push_back(k);
            }
        }
        for (unsigned i = 0; i < to_delete.size(); ++i) {
            var2val.remove(to_delete[i]);
            unfixed.push_back(to_delete[i]);
        }
        return to_delete.size();
    }

    //
    // Extract equalities that are congruent at the search level.
    // 
    unsigned context::extract_fixed_eqs(obj_map<expr, expr*>& var2val, expr_ref_vector& conseq) {
        TRACE("context", tout << "extract fixed consequences\n";);
        ast_manager& m = m_manager;
        ptr_vector<expr> to_delete;
        expr_ref fml(m), eq(m);
        obj_map<expr,expr*>::iterator it = var2val.begin(), end = var2val.end();
        for (; it != end; ++it) {
            expr* k = it->m_key;
            expr* v = it->m_value;
            if (!m.is_bool(k) && e_internalized(k) && e_internalized(v) && 
                get_enode(k)->get_root() == get_enode(v)->get_root()) {
                literal_vector literals;
                m_conflict_resolution->eq2literals(get_enode(v), get_enode(k), literals);
                uint_set s;
                for (unsigned i = 0; i < literals.size(); ++i) {
                    SASSERT(get_assign_level(literals[i]) <= get_search_level());
                    s |= m_antecedents.find(literals[i].var());
                }
                
                fml = m.mk_eq(k, v);
                fml = m.mk_implies(antecedent2fml(s), fml);
                conseq.push_back(fml);
                to_delete.push_back(k);

                for (unsigned i = 0; i < literals.size(); ++i) {
                    literals[i].neg();
                }
                eq = mk_eq_atom(k, v);
                internalize_formula(eq, false);
                literal lit(get_bool_var(eq), false);
                literals.push_back(lit);
                mk_clause(literals.size(), literals.c_ptr(), 0);
                TRACE("context", display_literals_verbose(tout, literals.size(), literals.c_ptr()););
            }
        }    
        for (unsigned i = 0; i < to_delete.size(); ++i) {
            var2val.remove(to_delete[i]);
        }
        return to_delete.size();
    }

    lbool context::get_consequences(expr_ref_vector const& assumptions, 
                                    expr_ref_vector const& vars, 
                                    expr_ref_vector& conseq, 
                                    expr_ref_vector& unfixed) {

        m_antecedents.reset();
        lbool is_sat = check(assumptions.size(), assumptions.c_ptr());
        if (is_sat != l_true) {
            return is_sat;
        }
        obj_map<expr, expr*> var2val;
        uint_set _assumptions;
        for (unsigned i = 0; i < assumptions.size(); ++i) {
            _assumptions.insert(get_literal(assumptions[i]).var());
        }
        model_ref mdl;
        get_model(mdl);
        ast_manager& m = m_manager;
        expr_ref_vector trail(m);
        model_evaluator eval(*mdl.get());
        expr_ref val(m);
        for (unsigned i = 0; i < vars.size(); ++i) {
            eval(vars[i], val);
            if (m.is_value(val)) {
                trail.push_back(val);
                var2val.insert(vars[i], val);
            } 
            else {
                unfixed.push_back(vars[i]);
            }
        }
        extract_fixed_consequences(0, var2val, _assumptions, conseq);
        unsigned num_units = assigned_literals().size();
        app_ref eq(m);
        TRACE("context", 
              tout << "vars: " << vars.size() << "\n";
              tout << "lits: " << num_units << "\n";);
        m_case_split_queue->init_search_eh();
        unsigned num_iterations = 0;
        unsigned model_threshold = 2;
        unsigned num_unfixed = 0;
        unsigned num_fixed_eqs = 0;
        unsigned num_reiterations = 0;
        while (!var2val.empty()) {
            obj_map<expr,expr*>::iterator it = var2val.begin();
            expr* e = it->m_key;
            expr* val = it->m_value;
            push_scope();

            TRACE("context", tout << "scope level: " << get_scope_level() << "\n";);
            SASSERT(!inconsistent());

            literal lit;
            if (m.is_bool(e)) {
                lit = literal(get_bool_var(e), m.is_true(val));
            }
            else {
                TRACE("context", tout << mk_pp(e, m) << " " << mk_pp(val, m) << "\n";);
                eq = mk_eq_atom(e, val);
                internalize_formula(eq, false);
                lit = literal(get_bool_var(eq), true);
            }
            assign(lit, b_justification::mk_axiom(), true);
            flet<bool> l(m_searching, true);
            while (true) {
                is_sat = bounded_search();
                TRACE("context", tout << "search result: " << is_sat << "\n";);
                if (is_sat != l_true && m_last_search_failure != OK) {
                    return is_sat;
                }
                if (is_sat == l_undef) {
                    TRACE("context", tout << "restart\n";);
                    inc_limits();
                    continue;
                }
                break;
            }
            if (is_sat == l_true && get_assignment(lit) == l_true) {
                var2val.erase(e);
                unfixed.push_back(e);
                TRACE("context", tout << mk_pp(e, m) << " is unfixed\n";);
            }
            else if (is_sat == l_true && get_assign_level(lit) > get_search_level()) {
                TRACE("context", tout << "Retry fixing: " << mk_pp(e, m) << "\n";);
                extract_fixed_consequences(num_units, var2val, _assumptions, conseq);
                ++num_reiterations;
                continue;
            }
            else {
                // The state can be labeled as inconsistent when the implied consequence does
                // not depend on assumptions, then the conflict level sits at the search level
                // which causes the conflict resolver to decide that the state is unsat.
                if (l_false == is_sat) {
                    SASSERT(inconsistent());
                    m_conflict = null_b_justification;
                    m_not_l = null_literal;
                }
                TRACE("context", tout << "Fixed: " << mk_pp(e, m) << " " << is_sat << "\n";
                      if (is_sat == l_false) display(tout););
                
            }
            ++num_iterations;

            bool apply_slow_pass = model_threshold <= num_iterations || num_iterations <= 2;
            if (apply_slow_pass && is_sat == l_true) {
                num_unfixed += delete_unfixed(var2val, unfixed);
                // The next time we check the model is after 1.5 additional iterations.
                model_threshold *= 3;
                model_threshold /= 2;                                
            }
            // repeat until we either have a model with negated literal or 
            // the literal is implied at base.

            extract_fixed_consequences(num_units, var2val, _assumptions, conseq);
            num_units = assigned_literals().size();
            if (apply_slow_pass) {
                num_fixed_eqs += extract_fixed_eqs(var2val, conseq);
                IF_VERBOSE(1, verbose_stream() << "(get-consequences"
                           << " iterations: " << num_iterations 
                           << " variables: " << var2val.size() 
                           << " fixed: " << conseq.size() 
                           << " unfixed: " << unfixed.size() 
                           << " fixed-eqs: " << num_fixed_eqs
                           << " unfixed-deleted: " << num_unfixed
                           << ")\n";);        
                TRACE("context", tout << "(get-consequences"
                      << " iterations: " << num_iterations 
                      << " variables: " << var2val.size() 
                      << " fixed: " << conseq.size() 
                      << " unfixed: " << unfixed.size() 
                      << " fixed-eqs: " << num_fixed_eqs
                      << " unfixed-deleted: " << num_unfixed
                      << ")\n";);        
            }
            TRACE("context", tout << "finishing " << mk_pp(e, m) << "\n";);
            SASSERT(!inconsistent());
            if (var2val.contains(e)) {
                TRACE("context", tout << "Fixed value to " << mk_pp(e, m) << " was not processed\n";);
                expr_ref fml(m);
                fml = m.mk_eq(e, var2val.find(e));
                if (!m_antecedents.contains(lit.var())) {
                    extract_fixed_consequences(lit, var2val, _assumptions, conseq);
                }
                fml = m.mk_implies(antecedent2fml(m_antecedents[lit.var()]), fml);
                conseq.push_back(fml);
                var2val.erase(e);
                SASSERT(get_assignment(lit) == l_false);
            }        
        }
        end_search();
        DEBUG_CODE(validate_consequences(assumptions, vars, conseq, unfixed););
        return l_true;
    }

    void context::validate_consequences(expr_ref_vector const& assumptions, expr_ref_vector const& vars, 
                                        expr_ref_vector const& conseq, expr_ref_vector const& unfixed) {
        ast_manager& m = m_manager;
        expr_ref tmp(m);
        SASSERT(!inconsistent());
        for (unsigned i = 0; i < conseq.size(); ++i) {
            push();            
            for (unsigned j = 0; j < assumptions.size(); ++j) {
                assert_expr(assumptions[j]);
            }
            TRACE("context", tout << "checking: " << mk_pp(conseq[i], m) << "\n";);
            tmp = m.mk_not(conseq[i]);
            assert_expr(tmp);
            lbool is_sat = check();
            SASSERT(is_sat != l_true);
            pop(1);
        }
        model_ref mdl;
        for (unsigned i = 0; i < unfixed.size(); ++i) {
            push();            
            for (unsigned j = 0; j < assumptions.size(); ++j) {
                assert_expr(assumptions[j]);
            }
            TRACE("context", tout << "checking unfixed: " << mk_pp(unfixed[i], m) << "\n";);
            lbool is_sat = check();            
            SASSERT(is_sat != l_false);
            if (is_sat == l_true) {
                get_model(mdl);
                mdl->eval(unfixed[i], tmp);
                if (m.is_value(tmp)) {
                    tmp = m.mk_not(m.mk_eq(unfixed[i], tmp));
                    assert_expr(tmp);
                    is_sat = check();
                    SASSERT(is_sat != l_false);
                }
            }
            pop(1);
        }
    }

}
