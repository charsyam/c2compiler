/* Copyright 2013 Bas van den Berg
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <assert.h>

#include <clang/Parse/ParseDiagnostic.h>
#include <clang/Sema/SemaDiagnostic.h>

#include "Scope.h"
#include "Package.h"
#include "Decl.h"
#include "Type.h"
#include "Expr.h"
#include "color.h"

using namespace C2;
using namespace clang;

GlobalScope::GlobalScope(const std::string& name_, const Pkgs& pkgs_, clang::DiagnosticsEngine& Diags_)
    : pkgName(name_)
    , allPackages(pkgs_)
    , Diags(Diags_)
{
    // add own package to scope
    const Package* myPackage = findAnyPackage(pkgName);
    addPackage(true, pkgName, myPackage);
}

void GlobalScope::addPackage(bool isLocal, const std::string& name_, const Package* pkg) {
    assert(pkg);
    if (isLocal) {
        locals.push_back(pkg);
    }
    packages[name_] = pkg;
}

const Package* GlobalScope::findPackage(const std::string& name) const {
    PackagesConstIter iter = packages.find(name);
    if (iter == packages.end()) return 0;
    return iter->second;
}

const Package* GlobalScope::findAnyPackage(const std::string& name) const {
    PkgsConstIter iter = allPackages.find(name);
    if (iter == allPackages.end()) return 0;
    return iter->second;
}

int GlobalScope::checkType(Type* type, bool used_public) {
    if (type->hasBuiltinBase()) return 0; // always ok

    switch (type->getKind()) {
    case Type::BUILTIN:
        assert(0);
        return 1;
    case Type::STRUCT:
    case Type::UNION:
        // TODO check members,
        fprintf(stderr, ANSI_BLUE"TODO check struct/union members"ANSI_NORMAL"\n");
        break;
    case Type::ENUM:
        // has no subtypes
        break;
    case Type::USER:
    case Type::FUNC:
    case Type::POINTER:
    case Type::ARRAY:
    case Type::QUALIFIER:
        return checkUserType(type->getBaseUserType(), used_public);
    }
    return 0;
}

int GlobalScope::checkUserType(IdentifierExpr* id, bool used_public) {
    if (id->pname != "") {
        // check if package exists
        const Package* pkg = findPackage(id->pname);
        if (!pkg) {
            // TODO use function
            PkgsConstIter iter = allPackages.find(id->pname);
            if (iter == allPackages.end()) {
                Diags.Report(id->ploc, diag::err_unknown_package) << id->pname;
            } else {
                Diags.Report(id->ploc, diag::err_package_not_used) << id->pname;
            }
            return 1;
        }

        // check Type
        Decl* symbol = pkg->findSymbol(id->name);
        if (!symbol) {
            Diags.Report(id->getLocation(), diag::err_unknown_typename) << id->getName();
            return 1;
        }
        TypeDecl* td = DeclCaster<TypeDecl>::getType(symbol);
        if (!td) {
            Diags.Report(id->getLocation(), diag::err_not_a_typename) << id->getName();
            return 1;
        }
        // if external package, check visibility
        if (isExternal(pkg) && !td->isPublic()) {
            Diags.Report(id->getLocation(), diag::err_not_public) << id->getName();
            return 1;
        }
        if (used_public && !isExternal(pkg) && !td->isPublic()) {
            Diags.Report(id->getLocation(), diag::err_non_public_type) << id->getName();
            return 1;
        }
    } else {
        ScopeResult res = findSymbol(id->name);
        if (!res.decl) {
            Diags.Report(id->getLocation(), diag::err_unknown_typename) << id->getName();
            return 1;
        }
        if (res.ambiguous) {
            Diags.Report(id->getLocation(), diag::err_ambiguous_symbol) << id->getName();
            // TODO show alternatives
            return 1;
        }
        if (res.external && !res.decl->isPublic()) {
            Diags.Report(id->getLocation(), diag::err_not_public) << id->getName();
            return 1;
        }
        TypeDecl* td = DeclCaster<TypeDecl>::getType(res.decl);
        if (!td) {
            Diags.Report(id->getLocation(), diag::err_not_a_typename) << id->getName();
            return 1;
        }
        if (used_public && !res.external && !td->isPublic()) {
            Diags.Report(id->getLocation(), diag::err_non_public_type) << id->getName();
            return 1;
        }
        // ok
    }
    return 0;
}

bool GlobalScope::isExternal(const Package* pkg) const {
    return (pkg->getName() != pkgName);
}

ScopeResult GlobalScope::findSymbol(const std::string& symbol) const {
    ScopeResult result;
    // return private symbol only if no public symbol is found
    // ambiguous may also be set with visible = false
    for (LocalsConstIter iter = locals.begin(); iter != locals.end(); ++iter) {
        const Package* pkg = *iter;
        Decl* decl = pkg->findSymbol(symbol);
        if (!decl) continue;

        bool external = isExternal(pkg);
        bool visible = !(external && !decl->isPublic());
        if (result.decl) {  // already found
            if (result.visible == visible) {
                result.ambiguous = true;
                if (result.visible) break;
                continue;
            }
            if (!result.visible) { // replace with visible symbol
                result.decl = decl;
                result.pkg = pkg;
                result.external = external;
                result.ambiguous = false;
                result.visible = visible;
            }
        } else {
            result.decl = decl;
            result.pkg = pkg;
            result.external = external;
            result.visible = visible;
        }
    }
    return result;
}



Scope::Scope(GlobalScope& globals_, Scope* parent_)
    : globals(globals_)
    , parent(parent_)
{}

ScopeResult Scope::findSymbol(const std::string& symbol) const {
    // search this scope
    ScopeResult result;
    for (DeclsConstIter iter = decls.begin(); iter != decls.end(); ++iter) {
        Decl* D = *iter;
        if (D->getName() == symbol) {
            result.decl = D;
            // TODO fill other result fields
            return result;
        }
    }

    // search parent or globals
    if (parent) return parent->findSymbol(symbol);
    else return globals.findSymbol(symbol);
}

void Scope::addDecl(Decl* d) {
    decls.push_back(d);
}
