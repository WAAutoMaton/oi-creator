/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2011 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact: Nokia Corporation (info@qt.nokia.com)
**
**
** GNU Lesser General Public License Usage
**
** This file may be used under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation and
** appearing in the file LICENSE.LGPL included in the packaging of this file.
** Please review the following information to ensure the GNU Lesser General
** Public License version 2.1 requirements will be met:
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights. These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** Other Usage
**
** Alternatively, this file may be used in accordance with the terms and
** conditions contained in a signed written agreement between you and Nokia.
**
** If you have questions regarding the use of this file, please contact
** Nokia at info@qt.nokia.com.
**
**************************************************************************/

#include "qmljsfindexportedcpptypes.h"

#include <qmljs/qmljsinterpreter.h>
#include <cplusplus/AST.h>
#include <cplusplus/TranslationUnit.h>
#include <cplusplus/ASTVisitor.h>
#include <cplusplus/ASTMatcher.h>
#include <cplusplus/ASTPatternBuilder.h>
#include <cplusplus/Overview.h>
#include <cplusplus/TypeOfExpression.h>
#include <cplusplus/Names.h>
#include <cplusplus/Literals.h>
#include <cplusplus/CoreTypes.h>
#include <cplusplus/Symbols.h>
#include <utils/qtcassert.h>

#include <QtCore/QDebug>

using namespace QmlJSTools;

namespace {
using namespace CPlusPlus;

class ExportedQmlType {
public:
    QString packageName;
    QString typeName;
    LanguageUtils::ComponentVersion version;
    Scope *scope;
    QString typeExpression;
};

class FindExportsVisitor : protected ASTVisitor
{
    CPlusPlus::Document::Ptr _doc;
    QList<ExportedQmlType> _exportedTypes;
    CompoundStatementAST *_compound;
    ASTMatcher _matcher;
    ASTPatternBuilder _builder;
    Overview _overview;

public:
    FindExportsVisitor(CPlusPlus::Document::Ptr doc)
        : ASTVisitor(doc->translationUnit())
        , _doc(doc)
        , _compound(0)
    {}

    QList<ExportedQmlType> operator()()
    {
        _exportedTypes.clear();
        accept(translationUnit()->ast());
        return _exportedTypes;
    }

protected:
    virtual bool visit(CompoundStatementAST *ast)
    {
        CompoundStatementAST *old = _compound;
        _compound = ast;
        accept(ast->statement_list);
        _compound = old;
        return false;
    }

    virtual bool visit(CallAST *ast)
    {
        IdExpressionAST *idExp = ast->base_expression->asIdExpression();
        if (!idExp || !idExp->name)
            return false;
        TemplateIdAST *templateId = idExp->name->asTemplateId();
        if (!templateId || !templateId->identifier_token)
            return false;

        // check the name
        const Identifier *templateIdentifier = translationUnit()->identifier(templateId->identifier_token);
        if (!templateIdentifier)
            return false;
        const QString callName = QString::fromUtf8(templateIdentifier->chars());
        if (callName != QLatin1String("qmlRegisterType"))
            return false;

        // must have a single typeid template argument
        if (!templateId->template_argument_list || !templateId->template_argument_list->value
                || templateId->template_argument_list->next)
            return false;
        TypeIdAST *typeId = templateId->template_argument_list->value->asTypeId();
        if (!typeId)
            return false;

        // must have four arguments
        if (!ast->expression_list
                || !ast->expression_list->value || !ast->expression_list->next
                || !ast->expression_list->next->value || !ast->expression_list->next->next
                || !ast->expression_list->next->next->value || !ast->expression_list->next->next->next
                || !ast->expression_list->next->next->next->value
                || ast->expression_list->next->next->next->next)
            return false;

        // last argument must be a string literal
        const StringLiteral *nameLit = 0;
        if (StringLiteralAST *nameAst = ast->expression_list->next->next->next->value->asStringLiteral())
            nameLit = translationUnit()->stringLiteral(nameAst->literal_token);
        if (!nameLit) {
            // disable this warning for now, we don't want to encourage using string literals if they don't mean to
            // in the future, we will also accept annotations for the qmlRegisterType arguments in comments
//            translationUnit()->warning(ast->expression_list->next->next->next->value->firstToken(),
//                                       "The type will only be available in Qt Creator's QML editors when the type name is a string literal");
            return false;
        }

        // if the first argument is a string literal, things are easy
        QString packageName;
        if (StringLiteralAST *packageAst = ast->expression_list->value->asStringLiteral()) {
            const StringLiteral *packageLit = translationUnit()->stringLiteral(packageAst->literal_token);
            packageName = QString::fromUtf8(packageLit->chars(), packageLit->size());
        }
        // as a special case, allow an identifier package argument if there's a
        // Q_ASSERT(QLatin1String(uri) == QLatin1String("actual uri"));
        // in the enclosing compound statement
        IdExpressionAST *uriName = ast->expression_list->value->asIdExpression();
        if (packageName.isEmpty() && uriName && _compound) {
            for (StatementListAST *it = _compound->statement_list; it; it = it->next) {
                StatementAST *stmt = it->value;

                packageName = nameOfUriAssert(stmt, uriName);
                if (!packageName.isEmpty())
                    break;
            }
        }

        // second and third argument must be integer literals
        const NumericLiteral *majorLit = 0;
        const NumericLiteral *minorLit = 0;
        if (NumericLiteralAST *majorAst = ast->expression_list->next->value->asNumericLiteral())
            majorLit = translationUnit()->numericLiteral(majorAst->literal_token);
        if (NumericLiteralAST *minorAst = ast->expression_list->next->next->value->asNumericLiteral())
            minorLit = translationUnit()->numericLiteral(minorAst->literal_token);

        // build the descriptor
        ExportedQmlType exportedType;
        exportedType.typeName = QString::fromUtf8(nameLit->chars(), nameLit->size());
        if (!packageName.isEmpty() && majorLit && minorLit && majorLit->isInt() && minorLit->isInt()) {
            exportedType.packageName = packageName;
            exportedType.version = LanguageUtils::ComponentVersion(
                        QString::fromUtf8(majorLit->chars(), majorLit->size()).toInt(),
                        QString::fromUtf8(minorLit->chars(), minorLit->size()).toInt());
        } else {
            // disable this warning, see above for details
//            translationUnit()->warning(ast->base_expression->firstToken(),
//                                       "The module will not be available in Qt Creator's QML editors because the uri and version numbers\n"
//                                       "cannot be determined by static analysis. The type will still be available globally.");
            exportedType.packageName = QmlJS::CppQmlTypes::defaultPackage;
        }

        // we want to do lookup later, so also store the surrounding scope
        unsigned line, column;
        translationUnit()->getTokenStartPosition(ast->firstToken(), &line, &column);
        exportedType.scope = _doc->scopeAt(line, column);

        // and the expression
        const Token begin = translationUnit()->tokenAt(typeId->firstToken());
        const Token last = translationUnit()->tokenAt(typeId->lastToken() - 1);
        exportedType.typeExpression = _doc->source().mid(begin.begin(), last.end() - begin.begin());

        _exportedTypes += exportedType;

        return false;
    }

private:
    QString stringOf(AST *ast)
    {
        const Token begin = translationUnit()->tokenAt(ast->firstToken());
        const Token last = translationUnit()->tokenAt(ast->lastToken() - 1);
        return _doc->source().mid(begin.begin(), last.end() - begin.begin());
    }

    ExpressionAST *skipStringCall(ExpressionAST *exp)
    {
        if (!exp)
            return 0;

        IdExpressionAST *callName = _builder.IdExpression();
        CallAST *call = _builder.Call(callName);
        if (!exp->match(call, &_matcher))
            return exp;

        const QString name = stringOf(callName);
        if (name != QLatin1String("QLatin1String")
                && name != QLatin1String("QString"))
            return exp;

        if (!call->expression_list || call->expression_list->next)
            return exp;

        return call->expression_list->value;
    }

    QString nameOfUriAssert(StatementAST *stmt, IdExpressionAST *uriName)
    {
        QString null;

        IdExpressionAST *outerCallName = _builder.IdExpression();
        BinaryExpressionAST *binary = _builder.BinaryExpression();
        // assert(... == ...);
        ExpressionStatementAST *pattern = _builder.ExpressionStatement(
                    _builder.Call(outerCallName, _builder.ExpressionList(
                                     binary)));

        if (!stmt->match(pattern, &_matcher)) {
            outerCallName = _builder.IdExpression();
            binary = _builder.BinaryExpression();
            // the expansion of Q_ASSERT(...),
            // ((!(... == ...)) ? qt_assert(...) : ...);
            pattern = _builder.ExpressionStatement(
                        _builder.NestedExpression(
                            _builder.ConditionalExpression(
                                _builder.NestedExpression(
                                    _builder.UnaryExpression(
                                        _builder.NestedExpression(
                                            binary))),
                                _builder.Call(outerCallName))));

            if (!stmt->match(pattern, &_matcher))
                return null;
        }

        const QString outerCall = stringOf(outerCallName);
        if (outerCall != QLatin1String("qt_assert")
                && outerCall != QLatin1String("assert")
                && outerCall != QLatin1String("Q_ASSERT"))
            return null;

        if (translationUnit()->tokenAt(binary->binary_op_token).kind() != T_EQUAL_EQUAL)
            return null;

        ExpressionAST *lhsExp = skipStringCall(binary->left_expression);
        ExpressionAST *rhsExp = skipStringCall(binary->right_expression);
        if (!lhsExp || !rhsExp)
            return null;

        StringLiteralAST *uriString = lhsExp->asStringLiteral();
        IdExpressionAST *uriArgName = lhsExp->asIdExpression();
        if (!uriString)
            uriString = rhsExp->asStringLiteral();
        if (!uriArgName)
            uriArgName = rhsExp->asIdExpression();
        if (!uriString || !uriArgName)
            return null;

        if (stringOf(uriArgName) != stringOf(uriName))
            return null;

        const StringLiteral *packageLit = translationUnit()->stringLiteral(uriString->literal_token);
        return QString::fromUtf8(packageLit->chars(), packageLit->size());
    }
};

static FullySpecifiedType stripPointerAndReference(const FullySpecifiedType &type)
{
    Type *t = type.type();
    while (t) {
        if (PointerType *ptr = t->asPointerType())
            t = ptr->elementType().type();
        else if (ReferenceType *ref = t->asReferenceType())
            t = ref->elementType().type();
        else
            break;
    }
    return FullySpecifiedType(t);
}

static QString toQmlType(const FullySpecifiedType &type)
{
    Overview overview;
    QString result = overview(stripPointerAndReference(type));
    if (result == QLatin1String("QString"))
        result = QLatin1String("string");
    return result;
}

static Class *lookupClass(const QString &expression, Scope *scope, TypeOfExpression &typeOf)
{
    QList<LookupItem> results = typeOf(expression, scope);
    Class *klass = 0;
    foreach (const LookupItem &item, results) {
        if (item.declaration()) {
            klass = item.declaration()->asClass();
            if (klass)
                return klass;
        }
    }
    return 0;
}

static void populate(LanguageUtils::FakeMetaObject::Ptr fmo, Class *klass,
                     QHash<Class *, LanguageUtils::FakeMetaObject::Ptr> *classes,
                     TypeOfExpression &typeOf)
{
    using namespace LanguageUtils;

    Overview namePrinter;

    classes->insert(klass, fmo);

    for (unsigned i = 0; i < klass->memberCount(); ++i) {
        Symbol *member = klass->memberAt(i);
        if (!member->name())
            continue;
        if (Function *func = member->type()->asFunctionType()) {
            if (!func->isSlot() && !func->isInvokable() && !func->isSignal())
                continue;
            FakeMetaMethod method(namePrinter(func->name()), toQmlType(func->returnType()));
            if (func->isSignal())
                method.setMethodType(FakeMetaMethod::Signal);
            else
                method.setMethodType(FakeMetaMethod::Slot);
            for (unsigned a = 0; a < func->argumentCount(); ++a) {
                Symbol *arg = func->argumentAt(a);
                QString name;
                if (arg->name())
                    name = namePrinter(arg->name());
                method.addParameter(name, toQmlType(arg->type()));
            }
            fmo->addMethod(method);
        }
        if (QtPropertyDeclaration *propDecl = member->asQtPropertyDeclaration()) {
            const FullySpecifiedType &type = propDecl->type();
            const bool isList = false; // ### fixme
            const bool isWritable = propDecl->flags() & QtPropertyDeclaration::WriteFunction;
            const bool isPointer = type.type() && type.type()->isPointerType();
            const int revision = 0; // ### fixme
            FakeMetaProperty property(
                        namePrinter(propDecl->name()),
                        toQmlType(type),
                        isList, isWritable, isPointer,
                        revision);
            fmo->addProperty(property);
        }
        if (QtEnum *qtEnum = member->asQtEnum()) {
            // find the matching enum
            Enum *e = 0;
            QList<LookupItem> result = typeOf(namePrinter(qtEnum->name()), klass);
            foreach (const LookupItem &item, result) {
                if (item.declaration()) {
                    e = item.declaration()->asEnum();
                    if (e)
                        break;
                }
            }
            if (!e)
                continue;

            FakeMetaEnum metaEnum(namePrinter(e->name()));
            for (unsigned j = 0; j < e->memberCount(); ++j) {
                Symbol *enumMember = e->memberAt(j);
                if (!enumMember->name())
                    continue;
                metaEnum.addKey(namePrinter(enumMember->name()), 0);
            }
            fmo->addEnum(metaEnum);
        }
    }

    // only single inheritance is supported
    if (klass->baseClassCount() > 0) {
        BaseClass *base = klass->baseClassAt(0);
        if (!base->name())
            return;

        const QString baseClassName = namePrinter(base->name());
        fmo->setSuperclassName(baseClassName);

        Class *baseClass = lookupClass(baseClassName, klass, typeOf);
        if (!baseClass)
            return;

        FakeMetaObject::Ptr baseFmo = classes->value(baseClass);
        if (!baseFmo) {
            baseFmo = FakeMetaObject::Ptr(new FakeMetaObject);
            populate(baseFmo, baseClass, classes, typeOf);
        }
    }
}

QList<LanguageUtils::FakeMetaObject::ConstPtr> exportedQmlObjects(
        const Document::Ptr &doc,
        const Snapshot &snapshot,
        const QList<ExportedQmlType> &exportedTypes)
{
    using namespace LanguageUtils;
    QList<FakeMetaObject::ConstPtr> exportedObjects;
    QHash<Class *, FakeMetaObject::Ptr> classes;

    if (exportedTypes.isEmpty())
        return exportedObjects;

    TypeOfExpression typeOf;
    typeOf.init(doc, snapshot);
    foreach (const ExportedQmlType &exportedType, exportedTypes) {
        FakeMetaObject::Ptr fmo(new FakeMetaObject);
        fmo->addExport(exportedType.typeName,
                       exportedType.packageName,
                       exportedType.version);
        exportedObjects += fmo;

        Class *klass = lookupClass(exportedType.typeExpression, exportedType.scope, typeOf);
        if (!klass)
            continue;

        // add the no-package export, so the cpp name can be used in properties
        Overview overview;
        fmo->addExport(overview(klass->name()), QmlJS::CppQmlTypes::cppPackage, ComponentVersion());

        populate(fmo, klass, &classes, typeOf);
    }

    return exportedObjects;
}

} // anonymous namespace

FindExportedCppTypes::FindExportedCppTypes(const CPlusPlus::Snapshot &snapshot)
    : m_snapshot(snapshot)
{
}

QList<LanguageUtils::FakeMetaObject::ConstPtr> FindExportedCppTypes::operator()(const CPlusPlus::Document::Ptr &document)
{
    QList<LanguageUtils::FakeMetaObject::ConstPtr> noResults;
    if (document->source().isEmpty()
            || !document->translationUnit()->ast())
        return noResults;

    FindExportsVisitor finder(document);
    QList<ExportedQmlType> exports = finder();
    if (exports.isEmpty())
        return noResults;

    return exportedQmlObjects(document, m_snapshot, exports);
}

bool FindExportedCppTypes::maybeExportsTypes(const Document::Ptr &document)
{
    if (!document->control())
        return false;
    const QByteArray qmlRegisterTypeToken("qmlRegisterType");
    if (document->control()->findIdentifier(
                qmlRegisterTypeToken.constData(), qmlRegisterTypeToken.size())) {
        return true;
    }
    return false;
}
