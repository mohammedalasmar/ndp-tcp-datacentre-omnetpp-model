//==========================================================================
// NEDRESOURCECACHE.CC -
//
//                     OMNeT++/OMNEST
//            Discrete System Simulation in C++
//
//==========================================================================

/*--------------------------------------------------------------*
  Copyright (C) 2002-2017 Andras Varga
  Copyright (C) 2006-2017 OpenSim Ltd.

  This file is distributed WITHOUT ANY WARRANTY. See the file
  `license' for details on this and other legal matters.
*--------------------------------------------------------------*/

#include <cstdio>
#include <cstring>
#include "common/fileutil.h"
#include "common/stringutil.h"
#include "common/fileglobber.h"
#include "common/patternmatcher.h"
#include "common/opp_ctype.h"
#include "nederror.h"
#include "nedexception.h"
#include "nedresourcecache.h"
#include "nedparser.h"
#include "nedxmlparser.h"
#include "neddtdvalidator.h"
#include "nedsyntaxvalidator.h"
#include "nedcrossvalidator.h"

using namespace omnetpp::common;

namespace omnetpp {
namespace nedxml {

// TODO collect errors in a NEDErrorStore?

NEDResourceCache::NEDResourceCache()
{
}

NEDResourceCache::~NEDResourceCache()
{
    for (auto & file : files)
        delete file.second;
    for (auto & nedType : nedTypes)
        delete nedType.second;
}

void NEDResourceCache::registerBuiltinDeclarations()
{
    // NED code to define built-in types
    const char *nedcode = NEDParser::getBuiltInDeclarations();

    NEDErrorStore errors;
    NEDParser parser(&errors);
    NEDElement *tree = parser.parseNEDText(nedcode, "built-in-declarations");
    if (errors.containsError()) {
        delete tree;
        throw NEDException(getFirstError(&errors).c_str());
    }

    // note: file must be called package.ned so that @namespace("") takes effect
    addFile("/[built-in-declarations]/package.ned", tree);
}

int NEDResourceCache::loadNedSourceFolder(const char *foldername)
{
    try {
        std::string canonicalFolderName = tidyFilename(toAbsolutePath(foldername).c_str(), true);
        std::string rootPackageName = determineRootPackageName(foldername);
        folderPackages[canonicalFolderName] = rootPackageName;
        return doLoadNedSourceFolder(foldername, rootPackageName.c_str());
    }
    catch (std::exception& e) {
        throw NEDException("Could not load NED sources from '%s': %s", foldername, e.what());
    }
}

int NEDResourceCache::doLoadNedSourceFolder(const char *foldername, const char *expectedPackage)
{
    PushDir pushDir(foldername);
    int count = 0;

    FileGlobber globber("*");
    const char *filename;
    while ((filename = globber.getNext()) != nullptr) {
        if (filename[0] == '.') {
            continue;  // ignore ".", "..", and dotfiles
        }
        if (isDirectory(filename)) {
            count += doLoadNedSourceFolder(filename, expectedPackage == nullptr ? nullptr : opp_join(".", expectedPackage, filename).c_str());
        }
        else if (opp_stringendswith(filename, ".ned")) {
            doLoadNedFileOrText(filename, nullptr, expectedPackage, false);
            count++;
        }
    }
    return count;
}

void NEDResourceCache::doLoadNedFileOrText(const char *nedfname, const char *nedtext, const char *expectedPackage, bool isXML)
{
    Assert(nedfname);
    if (getFile(nedfname))
        return;  // already loaded

    // parse file
    std::string nedfname2 = nedtext ? nedfname : tidyFilename(toAbsolutePath(nedfname).c_str());  // so that NedFileElement stores absolute file name
    NEDElement *tree = parseAndValidateNedFileOrText(nedfname2.c_str(), nedtext, isXML);
    Assert(tree);

    // check that declared package matches expected package
    PackageElement *packageDecl = (PackageElement *)tree->getFirstChildWithTag(NED_PACKAGE);
    std::string declaredPackage = packageDecl ? packageDecl->getName() : "";
    if (expectedPackage != nullptr && declaredPackage != std::string(expectedPackage))
        throw NEDException("Declared package '%s' does not match expected package '%s' in file %s",
                declaredPackage.c_str(), expectedPackage, nedfname);

    // register it
    addFile(nedfname2.c_str(), tree);
}

NEDElement *NEDResourceCache::parseAndValidateNedFileOrText(const char *fname, const char *nedtext, bool isXML)
{
    // load file
    NEDElement *tree = nullptr;
    NEDErrorStore errors;
    if (isXML) {
        if (nedtext)
            throw NEDException("loadNedText(): Parsing XML from string not supported");
        tree = parseXML(fname, &errors);
    }
    else {
        NEDParser parser(&errors);
        parser.setParseExpressions(true);
        parser.setStoreSource(false);
        if (nedtext)
            tree = parser.parseNEDText(nedtext, fname);
        else
            tree = parser.parseNEDFile(fname);
    }
    if (errors.containsError()) {
        delete tree;
        throw NEDException(getFirstError(&errors).c_str());
    }

    // DTD validation and additional syntax validation
    NEDDTDValidator dtdvalidator(&errors);
    dtdvalidator.validate(tree);
    if (errors.containsError()) {
        delete tree;
        throw NEDException(getFirstError(&errors, "NED internal DTD validation failure: ").c_str());
    }

    NEDSyntaxValidator syntaxvalidator(true, &errors);
    syntaxvalidator.validate(tree);
    if (errors.containsError()) {
        delete tree;
        throw NEDException(getFirstError(&errors).c_str());
    }
    return tree;
}

std::string NEDResourceCache::getFirstError(NEDErrorStore *errors, const char *prefix)
{
    // find first error
    int i;
    for (i = 0; i < errors->numMessages(); i++)
        if (errors->errorSeverityCode(i) == NED_SEVERITY_ERROR)
            break;
    Assert(i != errors->numMessages());

    // assemble message
    std::string message = errors->errorText(i);
    message[0] = opp_toupper(message[0]);
    std::string location = errors->errorLocation(i);
    if (opp_stringbeginswith(message.c_str(), "Syntax error, unexpected")) // this message is not really useful, replace it
        message = "Syntax error";
    if (!location.empty())
        message += ", at " + location;
    if (prefix)
        message = std::string(prefix) + message;
    return message;
}

void NEDResourceCache::loadNedFile(const char *nedfname, const char *expectedPackage, bool isXML)
{
    if (!nedfname)
        throw NEDException("loadNedFile(): File name is nullptr");

    doLoadNedFileOrText(nedfname, nullptr, expectedPackage, isXML);
    registerPendingNedTypes();
}

void NEDResourceCache::loadNedText(const char *name, const char *nedtext, const char *expectedPackage, bool isXML)
{
    if (!name)
        throw NEDException("loadNedText(): Name is nullptr");
    if (getFile(name))
        throw NEDException("loadNedText(): Name '%s' already used", name);

    doLoadNedFileOrText(name, nedtext, expectedPackage, isXML);
    registerPendingNedTypes();
}

bool NEDResourceCache::addFile(const char *fname, NEDElement *node)
{
    std::string key = tidyFilename(toAbsolutePath(fname).c_str());
    NEDFileMap::iterator it = files.find(key);
    if (it != files.end())
        return false;  // already added

    files[key] = node;

    PackageElement *packageDecl = (PackageElement *)node->getFirstChildWithTag(NED_PACKAGE);
    std::string packagePrefix = packageDecl ? packageDecl->getName() : "";
    if (!packagePrefix.empty())
        packagePrefix += ".";

    collectNedTypesFrom(node, packagePrefix, false);
    return true;
}

void NEDResourceCache::collectNedTypesFrom(NEDElement *node, const std::string& packagePrefix, bool areInnerTypes)
{
    for (NEDElement *child = node->getFirstChild(); child; child = child->getNextSibling()) {
        int tag = child->getTagCode();
        if (tag == NED_CHANNEL || tag == NED_CHANNEL_INTERFACE || tag == NED_SIMPLE_MODULE ||
            tag == NED_COMPOUND_MODULE || tag == NED_MODULE_INTERFACE ||
            tag == NED_ENUM || tag == NED_STRUCT || tag == NED_CLASS || tag == NED_MESSAGE)
        {
            std::string qname = packagePrefix + child->getAttribute("name");
            collectNedType(qname.c_str(), areInnerTypes, child);

            if (NEDElement *types = child->getFirstChildWithTag(NED_TYPES))
                collectNedTypesFrom(types, qname+".", true);
        }
    }
}

void NEDResourceCache::collectNedType(const char *qname, bool isInnerType, NEDElement *node)
{
    // we'll process it later, from doneLoadingNedFiles()
    pendingList.push_back(PendingNedType(qname, isInnerType, node));
}

bool NEDResourceCache::areDependenciesResolved(const char *qname, NEDElement *node)
{
    // check that all base types are resolved
    NEDLookupContext context = getParentContextOf(qname, node);
    for (NEDElement *child = node->getFirstChild(); child; child = child->getNextSibling()) {
        if (child->getTagCode() != NED_EXTENDS && child->getTagCode() != NED_INTERFACE_NAME)
            continue;

        const char *name = child->getAttribute("name");
        std::string qname = resolveNedType(context, name);
        if (qname.empty())
            return false;
    }
    return true;
}

void NEDResourceCache::doneLoadingNedFiles()
{
    // register NED types from all the files we've loaded
    registerPendingNedTypes();

    // if something was missing --> error
    if (!pendingList.empty()) {
        std::string unresolvedNames;
        for (int i = 0; i < (int)pendingList.size(); i++)
            unresolvedNames += std::string(i == 0 ? "" : ", ") + pendingList[i].qname;
        if (pendingList.size() == 1)
            throw NEDException(pendingList[0].node, "NED type '%s' could not be fully resolved due to a missing base type or interface", unresolvedNames.c_str());
        else
            throw NEDException("The following NED types could not be fully resolved due to a missing base type or interface: %s", unresolvedNames.c_str());
    }
}

void NEDResourceCache::registerPendingNedTypes()
{
    bool again = true;
    while (again) {
        again = false;
        for (int i = 0; i < (int)pendingList.size(); i++) {
            PendingNedType type = pendingList[i];
            if (areDependenciesResolved(type.qname.c_str(), type.node)) {
                if (lookup(type.qname.c_str()))
                    throw NEDException(type.node, "Redeclaration of %s %s", type.node->getTagName(), type.qname.c_str());

                registerNedType(type.qname.c_str(), type.isInnerType, type.node);
                pendingList.erase(pendingList.begin() + i--);
                again = true;
            }
        }
    }
}

void NEDResourceCache::registerNedType(const char *qname, bool isInnerType, NEDElement *node)
{
    NEDTypeInfo *decl = new NEDTypeInfo(this, qname, isInnerType, node);
    nedTypes[qname] = decl;
    nedTypeNames.clear();  // invalidate
}

NEDTypeInfo *NEDResourceCache::lookup(const char *qname) const
{
    // hash table lookup
    NEDTypeInfoMap::const_iterator i = nedTypes.find(qname);
    return i == nedTypes.end() ? nullptr : i->second;
}

NEDTypeInfo *NEDResourceCache::getDecl(const char *qname) const
{
    NEDTypeInfo *decl = lookup(qname);
    if (!decl)
        throw NEDException("NED declaration '%s' not found", qname);
    return decl;
}

NEDElement *NEDResourceCache::getFile(const char *fname) const
{
    // hash table lookup
    std::string key = tidyFilename(toAbsolutePath(fname).c_str());
    NEDFileMap::const_iterator i = files.find(key);
    return i == files.end() ? nullptr : i->second;
}

NedFileElement *NEDResourceCache::getParentPackageNedFile(NedFileElement *nedfile) const
{
    std::string nedfilename = tidyFilename(toAbsolutePath(nedfile->getFilename()).c_str(), true);
    std::string dir, fname;
    splitFileName(nedfilename.c_str(), dir, fname);
    dir = tidyFilename(dir.c_str(), true);

    std::string topDir = getNedSourceFolderForFolder(dir.c_str());
    if (topDir.empty())
        return nullptr;

    if (fname != "package.ned") {
        // get package.ned from same package
        NEDElement *e = getFile(tidyFilename(concatDirAndFile(dir.c_str(), "package.ned").c_str(), true).c_str());
        if (e)
            return (NedFileElement *)e;
    }

    // walk up in search for a package.ned
    while (dir != topDir) {
        // printf("%s   ~   %s\n", dir.c_str(), topDir.c_str());

        // chop last segment
        std::string parentDir, dummy;
        splitFileName(dir.c_str(), parentDir, dummy);
        Assert(dir != parentDir);  // we should exit via reaching the NED source folder
        dir = tidyFilename(parentDir.c_str(), true);

        // return package.ned from this dir, if exists
        NEDElement *e = getFile(tidyFilename(concatDirAndFile(dir.c_str(), "package.ned").c_str(), true).c_str());
        if (e)
            return (NedFileElement *)e;
    }
    return nullptr;
}

std::string NEDResourceCache::determineRootPackageName(const char *nedSourceFolderName)
{
    // determine if a package.ned file exists
    std::string packageNedFilename = std::string(nedSourceFolderName) + "/package.ned";
    FILE *f = fopen(packageNedFilename.c_str(), "r");
    if (!f)
        return "";
    fclose(f);

    // read package declaration from it
    NEDElement *tree = parseAndValidateNedFileOrText(packageNedFilename.c_str(), nullptr, false);
    Assert(tree);
    PackageElement *packageDecl = (PackageElement *)tree->getFirstChildWithTag(NED_PACKAGE);
    std::string result = packageDecl ? packageDecl->getName() : "";
    delete tree;
    return result;
}

static bool isPathPrefixOf(const char *prefix, const char *path)
{
    // note: both "prefix" and "path" must be canonical absolute paths for this to work
    int pathlen = strlen(path);
    int prefixlen = strlen(prefix);
    Assert(prefix[prefixlen-1] != '/' && path[pathlen-1] != '/');
    if (pathlen == prefixlen)
        return strcmp(path, prefix) == 0;
    else if (pathlen < prefixlen)
        return false;  // too short
    else if (strncmp(path, prefix, strlen(prefix)) != 0)
        return false;  // differ
    else
        return path[prefixlen] == '/';  // e.g. "/tmp/foo" is not prefix of "/tmp/foolish"
}

std::string NEDResourceCache::getNedSourceFolderForFolder(const char *folder) const
{
    // find NED source folder which is a prefix of folder.
    // note: this is unambiguous because nested NED source folders are not allowed
    std::string folderName = tidyFilename(toAbsolutePath(folder).c_str(), true);
    for (const auto & folderPackage : folderPackages)
        if (isPathPrefixOf(folderPackage.first.c_str(), folderName.c_str()))
            return folderPackage.first;

    return "";
}

std::string NEDResourceCache::getNedPackageForFolder(const char *folder) const
{
    std::string sourceFolder = getNedSourceFolderForFolder(folder);
    if (sourceFolder.empty())
        return "";

    std::string folderName = tidyFilename(toAbsolutePath(folder).c_str(), true);
    std::string suffix = folderName.substr(sourceFolder.size());
    if (suffix.length() > 0 && suffix[0] == '/')
        suffix = suffix.substr(1);
    std::string subpackage = opp_replacesubstring(suffix, "/", ".", true);
    return opp_join(".", const_cast<StringMap&>(folderPackages)[sourceFolder].c_str(), subpackage.c_str());
}

NEDLookupContext NEDResourceCache::getParentContextOf(const char *qname, NEDElement *node)
{
    NEDElement *contextnode = node->getParent();
    if (contextnode->getTagCode() == NED_TYPES)
        contextnode = contextnode->getParent();
    const char *lastdot = strrchr(qname, '.');
    std::string contextqname = !lastdot ? "" : std::string(qname, lastdot-qname);
    return NEDLookupContext(contextnode, contextqname.c_str());
}

std::string NEDResourceCache::resolveNedType(const NEDLookupContext& context, const char *nedtypename, INEDTypeNames *qnames)
{
    // note: this method is to be kept consistent with NEDResources.lookupNedType() in the Java code
    // note2: partially qualified names are not supported: name must be either simplename or fully qualified
    if (!strchr(nedtypename, '.')) {
        // no dot: name is an unqualified name (simple name); so, it can be:
        // (a) inner type, (b) an exactly imported type, (c) from the same package, (d) a wildcard imported type

        // inner type?
        if (context.element->getTagCode() == NED_COMPOUND_MODULE) {
            std::string qname = context.qname;
            NEDElement *topLevelCompoundModule = context.element->getParent()->getParentWithTag(NED_COMPOUND_MODULE);
            if (topLevelCompoundModule) {
                // if context is already an inner type, look up nedtypename in its enclosing toplevel NED type
                int index = qname.rfind('.');
                Assert(index != -1);
                qname.replace(index, qname.length() - index, "");
            }
            /* TODO new code for the above, using NEDTypeInfo:
               NEDTypeInfo *contextNedType = getDecl(context.qname.c_str()) const;
               if (contextNedType->isInnerType())
                contextNedType = getDecl(contextNedType->getEnclosingTypeName());
               qname = std::string(contextNedType->getFullName()) + "." + nedtypename;
             */
            qname = qname + "." + nedtypename;
            if (qnames->contains(qname.c_str()))
                return qname;
            // TODO: try with ancestor types (i.e. maybe nedtypename is an inherited inner type)
        }

        NedFileElement *nedfileNode = dynamic_cast<NedFileElement *>(context.element->getParentWithTag(NED_NED_FILE));

        // collect imports, for convenience
        std::vector<const char *> imports;
        for (ImportElement *import = nedfileNode->getFirstImportChild(); import; import = import->getNextImportSibling())
            imports.push_back(import->getImportSpec());

        // exactly imported type?
        // try a shortcut first: if the import doesn't contain wildcards
        std::string dot_nedtypename = std::string(".")+nedtypename;
        for (auto & import : imports)
            if (qnames->contains(import) && (opp_stringendswith(import, dot_nedtypename.c_str()) || strcmp(import, nedtypename) == 0))
                return import;


        // from the same package?
        PackageElement *packageNode = nedfileNode->getFirstPackageChild();
        const char *packageName = packageNode ? packageNode->getName() : "";
        std::string qname = opp_isempty(packageName) ? nedtypename : std::string(packageName) + "." + nedtypename;
        if (qnames->contains(qname.c_str()))
            return qname;

        // try harder, using wildcards
        for (auto & import : imports) {
            if (PatternMatcher::containsWildcards(import)) {
                PatternMatcher importpattern(import, true, true, true);
                for (int j = 0; j < qnames->size(); j++) {
                    const char *qname = qnames->get(j);
                    if ((opp_stringendswith(qname, dot_nedtypename.c_str()) || strcmp(qname, nedtypename) == 0))
                        if (importpattern.matches(qname))
                            return qname;

                }
            }
        }
    }
    else {
        // fully qualified name?
        if (qnames->contains(nedtypename))
            return nedtypename;
    }

    return "";
}

const std::vector<std::string>& NEDResourceCache::getTypeNames() const
{
    if (nedTypeNames.empty() && !nedTypes.empty()) {
        // fill in nedTypeNames vector
        for (const auto & nedType : nedTypes)
            nedTypeNames.push_back(nedType.first);
    }
    return nedTypeNames;
}

}  // namespace nedxml
}  // namespace omnetpp

