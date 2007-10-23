//===--- IdentifierTable.cpp - Hash table for identifier lookup -----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by Chris Lattner and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the IdentifierInfo, IdentifierVisitor, and
// IdentifierTable interfaces.
//
//===----------------------------------------------------------------------===//

#include "clang/Basic/IdentifierTable.h"
#include "clang/Basic/LangOptions.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Bitcode/Serialization.h"

using namespace clang;

//===----------------------------------------------------------------------===//
// IdentifierInfo Implementation
//===----------------------------------------------------------------------===//

IdentifierInfo::IdentifierInfo() {
  TokenID = tok::identifier;
  ObjCID = tok::objc_not_keyword;
  BuiltinID = 0;
  HasMacro = false;
  IsExtension = false;
  IsPoisoned = false;
  IsOtherTargetMacro = false;
  IsCPPOperatorKeyword = false;
  IsNonPortableBuiltin = false;
  FETokenInfo = 0;
}

//===----------------------------------------------------------------------===//
// IdentifierTable Implementation
//===----------------------------------------------------------------------===//

IdentifierTable::IdentifierTable(const LangOptions &LangOpts)
  // Start with space for 8K identifiers.
  : HashTable(8192) {

  // Populate the identifier table with info about keywords for the current
  // language.
  AddKeywords(LangOpts);
}

// This cstor is intended to be used only for serialization.
IdentifierTable::IdentifierTable() : HashTable(8192) {}

//===----------------------------------------------------------------------===//
// Language Keyword Implementation
//===----------------------------------------------------------------------===//

/// AddKeyword - This method is used to associate a token ID with specific
/// identifiers because they are language keywords.  This causes the lexer to
/// automatically map matching identifiers to specialized token codes.
///
/// The C90/C99/CPP/CPP0x flags are set to 0 if the token should be
/// enabled in the specified langauge, set to 1 if it is an extension
/// in the specified language, and set to 2 if disabled in the
/// specified language.
static void AddKeyword(const char *Keyword, unsigned KWLen,
                       tok::TokenKind TokenCode,
                       int C90, int C99, int CXX, int CXX0x,
                       const LangOptions &LangOpts, IdentifierTable &Table) {
  int Flags = LangOpts.CPlusPlus ? (LangOpts.CPlusPlus0x? CXX0x : CXX)
                                 : (LangOpts.C99 ? C99 : C90);
  
  // Don't add this keyword if disabled in this language or if an extension
  // and extensions are disabled.
  if (Flags + LangOpts.NoExtensions >= 2) return;
  
  IdentifierInfo &Info = Table.get(Keyword, Keyword+KWLen);
  Info.setTokenID(TokenCode);
  Info.setIsExtensionToken(Flags == 1);
}

static void AddAlias(const char *Keyword, unsigned KWLen,
                     const char *AliaseeKeyword, unsigned AliaseeKWLen,
                     const LangOptions &LangOpts, IdentifierTable &Table) {
  IdentifierInfo &AliasInfo = Table.get(Keyword, Keyword+KWLen);
  IdentifierInfo &AliaseeInfo = Table.get(AliaseeKeyword,
                                          AliaseeKeyword+AliaseeKWLen);
  AliasInfo.setTokenID(AliaseeInfo.getTokenID());
  AliasInfo.setIsExtensionToken(AliaseeInfo.isExtensionToken());
}  

/// AddCXXOperatorKeyword - Register a C++ operator keyword alternative
/// representations.
static void AddCXXOperatorKeyword(const char *Keyword, unsigned KWLen,
                                  tok::TokenKind TokenCode,
                                  IdentifierTable &Table) {
  IdentifierInfo &Info = Table.get(Keyword, Keyword + KWLen);
  Info.setTokenID(TokenCode);
  Info.setIsCPlusPlusOperatorKeyword();
}

/// AddObjCKeyword - Register an Objective-C @keyword like "class" "selector" or 
/// "property".
static void AddObjCKeyword(tok::ObjCKeywordKind ObjCID, 
                           const char *Name, unsigned NameLen,
                           IdentifierTable &Table) {
  Table.get(Name, Name+NameLen).setObjCKeywordID(ObjCID);
}

/// AddKeywords - Add all keywords to the symbol table.
///
void IdentifierTable::AddKeywords(const LangOptions &LangOpts) {
  enum {
    C90Shift = 0,
    EXTC90   = 1 << C90Shift,
    NOTC90   = 2 << C90Shift,
    C99Shift = 2,
    EXTC99   = 1 << C99Shift,
    NOTC99   = 2 << C99Shift,
    CPPShift = 4,
    EXTCPP   = 1 << CPPShift,
    NOTCPP   = 2 << CPPShift,
    CPP0xShift = 6,
    EXTCPP0x   = 1 << CPP0xShift,
    NOTCPP0x   = 2 << CPP0xShift,
    Mask     = 3
  };
  
  // Add keywords and tokens for the current language.
#define KEYWORD(NAME, FLAGS) \
  AddKeyword(#NAME, strlen(#NAME), tok::kw_ ## NAME,  \
             ((FLAGS) >> C90Shift) & Mask, \
             ((FLAGS) >> C99Shift) & Mask, \
             ((FLAGS) >> CPPShift) & Mask, \
             ((FLAGS) >> CPP0xShift) & Mask, LangOpts, *this);
#define ALIAS(NAME, TOK) \
  AddAlias(NAME, strlen(NAME), #TOK, strlen(#TOK), LangOpts, *this);
#define CXX_KEYWORD_OPERATOR(NAME, ALIAS) \
  if (LangOpts.CXXOperatorNames)          \
    AddCXXOperatorKeyword(#NAME, strlen(#NAME), tok::ALIAS, *this);
#define OBJC1_AT_KEYWORD(NAME) \
  if (LangOpts.ObjC1)          \
    AddObjCKeyword(tok::objc_##NAME, #NAME, strlen(#NAME), *this);
#define OBJC2_AT_KEYWORD(NAME) \
  if (LangOpts.ObjC2)          \
    AddObjCKeyword(tok::objc_##NAME, #NAME, strlen(#NAME), *this);
#include "clang/Basic/TokenKinds.def"
}

tok::PPKeywordKind IdentifierInfo::getPPKeywordID() const {
  // We use a perfect hash function here involving the length of the keyword,
  // the first and third character.  For preprocessor ID's there are no
  // collisions (if there were, the switch below would complain about duplicate
  // case values).  Note that this depends on 'if' being null terminated.
  
#define HASH(LEN, FIRST, THIRD) \
  (LEN << 5) + (((FIRST-'a') + (THIRD-'a')) & 31)
#define CASE(LEN, FIRST, THIRD, NAME) \
  case HASH(LEN, FIRST, THIRD): \
    return memcmp(Name, #NAME, LEN) ? tok::pp_not_keyword : tok::pp_ ## NAME
    
  unsigned Len = getLength();
  if (Len < 2) return tok::pp_not_keyword;
  const char *Name = getName();
  switch (HASH(Len, Name[0], Name[2])) {
  default: return tok::pp_not_keyword;
  CASE( 2, 'i', '\0', if);
  CASE( 4, 'e', 'i', elif);
  CASE( 4, 'e', 's', else);
  CASE( 4, 'l', 'n', line);
  CASE( 4, 's', 'c', sccs);
  CASE( 5, 'e', 'd', endif);
  CASE( 5, 'e', 'r', error);
  CASE( 5, 'i', 'e', ident);
  CASE( 5, 'i', 'd', ifdef);
  CASE( 5, 'u', 'd', undef);

  CASE( 6, 'a', 's', assert);
  CASE( 6, 'd', 'f', define);
  CASE( 6, 'i', 'n', ifndef);
  CASE( 6, 'i', 'p', import);
  CASE( 6, 'p', 'a', pragma);

  CASE( 7, 'd', 'f', defined);
  CASE( 7, 'i', 'c', include);
  CASE( 7, 'w', 'r', warning);

  CASE( 8, 'u', 'a', unassert);
  CASE(12, 'i', 'c', include_next);
  CASE(13, 'd', 'f', define_target);
  CASE(19, 'd', 'f', define_other_target);
#undef CASE
#undef HASH
  }
}

//===----------------------------------------------------------------------===//
// Stats Implementation
//===----------------------------------------------------------------------===//

/// PrintStats - Print statistics about how well the identifier table is doing
/// at hashing identifiers.
void IdentifierTable::PrintStats() const {
  unsigned NumBuckets = HashTable.getNumBuckets();
  unsigned NumIdentifiers = HashTable.getNumItems();
  unsigned NumEmptyBuckets = NumBuckets-NumIdentifiers;
  unsigned AverageIdentifierSize = 0;
  unsigned MaxIdentifierLength = 0;
  
  // TODO: Figure out maximum times an identifier had to probe for -stats.
  for (llvm::StringMap<IdentifierInfo, llvm::BumpPtrAllocator>::const_iterator
       I = HashTable.begin(), E = HashTable.end(); I != E; ++I) {
    unsigned IdLen = I->getKeyLength();
    AverageIdentifierSize += IdLen;
    if (MaxIdentifierLength < IdLen)
      MaxIdentifierLength = IdLen;
  }
  
  fprintf(stderr, "\n*** Identifier Table Stats:\n");
  fprintf(stderr, "# Identifiers:   %d\n", NumIdentifiers);
  fprintf(stderr, "# Empty Buckets: %d\n", NumEmptyBuckets);
  fprintf(stderr, "Hash density (#identifiers per bucket): %f\n",
          NumIdentifiers/(double)NumBuckets);
  fprintf(stderr, "Ave identifier length: %f\n",
          (AverageIdentifierSize/(double)NumIdentifiers));
  fprintf(stderr, "Max identifier length: %d\n", MaxIdentifierLength);
  
  // Compute statistics about the memory allocated for identifiers.
  HashTable.getAllocator().PrintStats();
}

//===----------------------------------------------------------------------===//
// SelectorTable Implementation
//===----------------------------------------------------------------------===//

unsigned llvm::DenseMapInfo<clang::Selector>::getHashValue(clang::Selector S) {
  return DenseMapInfo<void*>::getHashValue(S.getAsOpaquePtr());
}


/// MultiKeywordSelector - One of these variable length records is kept for each
/// selector containing more than one keyword. We use a folding set
/// to unique aggregate names (keyword selectors in ObjC parlance). Access to 
/// this class is provided strictly through Selector.
namespace clang {
class MultiKeywordSelector : public llvm::FoldingSetNode {
public:  
  unsigned NumArgs;

  // Constructor for keyword selectors.
  MultiKeywordSelector(unsigned nKeys, IdentifierInfo **IIV) {
    assert((nKeys > 1) && "not a multi-keyword selector");
    NumArgs = nKeys;
    // Fill in the trailing keyword array.
    IdentifierInfo **KeyInfo = reinterpret_cast<IdentifierInfo **>(this+1);
    for (unsigned i = 0; i != nKeys; ++i)
      KeyInfo[i] = IIV[i];
  }
  // getName - Derive the full selector name and return it.
  std::string getName() const;
    
  unsigned getNumArgs() const { return NumArgs; }
  
  typedef IdentifierInfo *const *keyword_iterator;
  keyword_iterator keyword_begin() const {
    return reinterpret_cast<keyword_iterator>(this+1);
  }
  keyword_iterator keyword_end() const { 
    return keyword_begin()+NumArgs; 
  }
  IdentifierInfo *getIdentifierInfoForSlot(unsigned i) const {
    assert(i < NumArgs && "getIdentifierInfoForSlot(): illegal index");
    return keyword_begin()[i];
  }
  static void Profile(llvm::FoldingSetNodeID &ID, 
                      keyword_iterator ArgTys, unsigned NumArgs) {
    ID.AddInteger(NumArgs);
    for (unsigned i = 0; i != NumArgs; ++i)
      ID.AddPointer(ArgTys[i]);
  }
  void Profile(llvm::FoldingSetNodeID &ID) {
    Profile(ID, keyword_begin(), NumArgs);
  }
};
} // end namespace clang.

unsigned Selector::getNumArgs() const {
  unsigned IIF = getIdentifierInfoFlag();
  if (IIF == ZeroArg)
    return 0;
  if (IIF == OneArg)
    return 1;
  // We point to a MultiKeywordSelector (pointer doesn't contain any flags).
  MultiKeywordSelector *SI = reinterpret_cast<MultiKeywordSelector *>(InfoPtr);
  return SI->getNumArgs(); 
}

IdentifierInfo *Selector::getIdentifierInfoForSlot(unsigned argIndex) const {
  if (IdentifierInfo *II = getAsIdentifierInfo()) {
    assert(argIndex == 0 && "illegal keyword index");
    return II;
  }
  // We point to a MultiKeywordSelector (pointer doesn't contain any flags).
  MultiKeywordSelector *SI = reinterpret_cast<MultiKeywordSelector *>(InfoPtr);
  return SI->getIdentifierInfoForSlot(argIndex);
}

std::string MultiKeywordSelector::getName() const {
  std::string Result;
  unsigned Length = 0;
  for (keyword_iterator I = keyword_begin(), E = keyword_end(); I != E; ++I) {
    if (*I)
      Length += (*I)->getLength();
    ++Length;  // :
  }
  
  Result.reserve(Length);
  
  for (keyword_iterator I = keyword_begin(), E = keyword_end(); I != E; ++I) {
    if (*I)
      Result.insert(Result.end(), (*I)->getName(),
                    (*I)->getName()+(*I)->getLength());
    Result.push_back(':');
  }
  
  return Result;
}

std::string Selector::getName() const {
  if (IdentifierInfo *II = getAsIdentifierInfo()) {
    if (getNumArgs() == 0)
      return II->getName();
    
    std::string Res = II->getName();
    Res += ":";
    return Res;
  }
  
  // We have a multiple keyword selector (no embedded flags).
  return reinterpret_cast<MultiKeywordSelector *>(InfoPtr)->getName();
}


Selector SelectorTable::getSelector(unsigned nKeys, IdentifierInfo **IIV) {
  if (nKeys < 2)
    return Selector(IIV[0], nKeys);
  
  llvm::FoldingSet<MultiKeywordSelector> *SelTab;
  
  SelTab = static_cast<llvm::FoldingSet<MultiKeywordSelector> *>(Impl);
    
  // Unique selector, to guarantee there is one per name.
  llvm::FoldingSetNodeID ID;
  MultiKeywordSelector::Profile(ID, IIV, nKeys);

  void *InsertPos = 0;
  if (MultiKeywordSelector *SI = SelTab->FindNodeOrInsertPos(ID, InsertPos))
    return Selector(SI);
  
  // MultiKeywordSelector objects are not allocated with new because they have a
  // variable size array (for parameter types) at the end of them.
  MultiKeywordSelector *SI = 
    (MultiKeywordSelector*)malloc(sizeof(MultiKeywordSelector) + 
                                  nKeys*sizeof(IdentifierInfo *));
  new (SI) MultiKeywordSelector(nKeys, IIV);
  SelTab->InsertNode(SI, InsertPos);
  return Selector(SI);
}

SelectorTable::SelectorTable() {
  Impl = new llvm::FoldingSet<MultiKeywordSelector>;
}

SelectorTable::~SelectorTable() {
  delete static_cast<llvm::FoldingSet<MultiKeywordSelector> *>(Impl);
}

//===----------------------------------------------------------------------===//
// Serialization for IdentifierInfo and IdentifierTable.
//===----------------------------------------------------------------------===//

void llvm::SerializeTrait<IdentifierInfo>::Serialize(llvm::Serializer& S,
                                                    const IdentifierInfo& I) {

  S.Emit<tok::TokenKind>(I.getTokenID());
  S.EmitInt(I.getBuiltinID(),9);
  S.Emit<tok::ObjCKeywordKind>(I.getObjCKeywordID());  
  S.Emit(I.hasMacroDefinition());
  S.Emit(I.isExtensionToken());
  S.Emit(I.isPoisoned());
  S.Emit(I.isOtherTargetMacro());
  S.Emit(I.isCPlusPlusOperatorKeyword());
  S.Emit(I.isNonPortableBuiltin());   
}

void llvm::SerializeTrait<IdentifierInfo>::Deserialize(llvm::Deserializer& D,
                                                       IdentifierInfo& I) {
  tok::TokenKind X;
  I.setTokenID(D.Read<tok::TokenKind>(X));

  I.setBuiltinID(D.ReadInt(9));  
  
  tok::ObjCKeywordKind Y;
  I.setObjCKeywordID(D.Read<tok::ObjCKeywordKind>(Y));
  
  I.setHasMacroDefinition(D.ReadBool());
  I.setIsExtensionToken(D.ReadBool());
  I.setIsPoisoned(D.ReadBool());
  I.setIsOtherTargetMacro(D.ReadBool());
  I.setIsCPlusPlusOperatorKeyword(D.ReadBool());
  I.setNonPortableBuiltin(D.ReadBool());
}

void llvm::SerializeTrait<IdentifierTable>::Serialize(llvm::Serializer& S,
                                                      const IdentifierTable& T){
  S.Emit<unsigned>(T.size());
  
  for (clang::IdentifierTable::iterator I=T.begin(), E=T.end(); I != E; ++I) {
    S.EmitCString(I->getKeyData());
    S.Emit(I->getValue());
  }
}

void llvm::SerializeTrait<IdentifierTable>::Deserialize(llvm::Deserializer& D,
                                                        IdentifierTable& T) {
  unsigned len = D.ReadInt();
  std::vector<char> buff;
  buff.reserve(200);
  
  for (unsigned i = 0; i < len; ++i) {
    D.ReadCString(buff);
    IdentifierInfo& Info = T.get(&buff[0],&buff[0]+buff.size());
    D.Read(Info);
  }
}
  

