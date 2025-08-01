/*
* Copyright (c) 2003-2025 Rony Shapiro <ronys@pwsafe.org>.
* All rights reserved. Use of the code is allowed under the
* Artistic License 2.0 terms, as specified in the LICENSE file
* distributed with this code, or available from
* http://www.opensource.org/licenses/artistic-license-2.0.php
*/
/// \file ItemData.cpp
//-----------------------------------------------------------------------------

#include "ItemData.h"
#include "crypto/BlowFish.h"
#include "crypto/TwoFish.h"
#include "UTF8Conv.h"
#include "PWSprefs.h"
#include "VerifyFormat.h"
#include "PWHistory.h"
#include "Util.h"
#include "StringXStream.h"
#include "core.h"
#include "PWSfile.h"
#include "PWSfileV4.h"
#include "PWStime.h"
#include "os/pws_tchar.h"
#include "os/utf8conv.h"

#include <ctime>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <functional>
#include <array>

using namespace std;
using pws_os::CUUID;

//-----------------------------------------------------------------------------
// Helper functions.

// ResolvePlaceholderEligibleField: For eligible fields, if an entry is an
// alias or shortcut, resolve to a placeholder value, otherwise resolve to
// the actual value from the actual entry.
StringX ResolvePlaceholderEligibleField(const CItemData* pcientry, const CItemData* pcibase, std::function<StringX()> getter_func)
{
  ASSERT(pcientry);
  const CItemData::EntryType et = pcientry->GetEntryType();
  if (et == CItemData::EntryType::ET_ALIAS || et == CItemData::EntryType::ET_SHORTCUT) {
    // Alias or Shortcut so return appropriate placeholder value.
    ASSERT(pcibase);
    const StringX csPlaceholderBase =
      pcibase->GetGroup() + _T(":") +
      pcibase->GetTitle() + _T(":") +
      pcibase->GetUser();
    const StringX csPlaceholderAlias = _T("[[") + csPlaceholderBase + _T("]]");
    const StringX csPlaceholderShortcut = _T("[~") + csPlaceholderBase + _T("~]");
    const StringX csPlaceholderToUse = et == CItemData::EntryType::ET_ALIAS ? csPlaceholderAlias : csPlaceholderShortcut;
    return csPlaceholderToUse;
  }
  // Neither alias/shortcut, placeholder not needed, return actual field value.
  return getter_func();
}

//-----------------------------------------------------------------------------
// Constructors

CItemData::CItemData()
  : m_entrytype(ET_NORMAL), m_entrystatus(ES_CLEAN)
{
}

CItemData::CItemData(const CItemData &that) :
  CItem(that), m_entrytype(that.m_entrytype), m_entrystatus(that.m_entrystatus)
{
}

CItemData::~CItemData()
{
}

CItemData& CItemData::operator=(const CItemData &that)
{
  if (this != &that) { // Check for self-assignment
    CItem::operator=(that);
    m_entrytype = that.m_entrytype;
    m_entrystatus = that.m_entrystatus;
  }
  return *this;
}

void CItemData::Clear()
{
  CItem::Clear();
  m_entrytype = ET_NORMAL;
  m_entrystatus = ES_CLEAN;
}

bool CItemData::operator==(const CItemData &that) const
{
  return (m_entrytype == that.m_entrytype &&
          m_entrystatus == that.m_entrystatus &&
          CItem::operator==(that));
}

void CItemData::ParseSpecialPasswords()
{
  // For V3 records, the Base UUID and dependent type (shortcut or alias)
  // is encoded in the password field. 
  // If the password isn't in the encoded format, this is a no-op
  // If it is, then this 'normalizes' the entry record to be the same
  // as a V4 one.

  const StringX csMyPassword = GetPassword();
  if (csMyPassword.length() == 36) { // look for "[[uuid]]" or "[~uuid~]"
    StringX cs_possibleUUID = csMyPassword.substr(2, 32); // try to extract uuid
    ToLower(cs_possibleUUID);
    if (((csMyPassword.substr(0,2) == _T("[[") &&
          csMyPassword.substr(csMyPassword.length() - 2) == _T("]]")) ||
         (csMyPassword.substr(0, 2) == _T("[~") &&
          csMyPassword.substr(csMyPassword.length() - 2) == _T("~]"))) &&
        cs_possibleUUID.find_first_not_of(_T("0123456789abcdef")) == StringX::npos) {
      CUUID buuid(cs_possibleUUID.c_str());
      SetUUID(buuid, BASEUUID);
      uuid_array_t uuid;
      GetUUID(uuid);
      FieldType ft = UUID;
      if (csMyPassword.substr(0, 2) == _T("[[")) {
        ft = ALIASUUID;
      } else if (csMyPassword.substr(0, 2) == _T("[~")) {
        ft = SHORTCUTUUID;
      } else {
        ASSERT(0);
      }
      ClearField(UUID);
      SetUUID(uuid, ft);
    }
  }
}

bool CItemData::HasUUID() const
{
  return (((m_entrytype == ET_NORMAL ||
            m_entrytype == ET_ALIASBASE ||
            m_entrytype == ET_SHORTCUTBASE) && IsFieldSet(UUID)) ||
          (m_entrytype == ET_ALIAS && IsFieldSet(ALIASUUID)) ||
          (m_entrytype == ET_SHORTCUT && IsFieldSet(SHORTCUTUUID)));
}

void CItemData::SetSpecialPasswords()
{
  // For writing a record in V3 format

  if (IsDependent()) {
    ASSERT(IsFieldSet(BASEUUID));
    const CUUID base_uuid(GetUUID(BASEUUID));
    ASSERT(base_uuid != CUUID::NullUUID());
    ASSERT(base_uuid != GetUUID()); // not self-referential!
    StringX uuid_str;

    if (IsAlias()) {
      uuid_str = _T("[[");
      uuid_str += base_uuid;
      uuid_str += _T("]]");
    } else if (IsShortcut()) {
      uuid_str = _T("[~");
      uuid_str += base_uuid;
      uuid_str += _T("~]");
    } else
      ASSERT(0);

    SetPassword(uuid_str);
  } // IsDependent()
}

int CItemData::Read(PWSfile *in)
{
  int status = PWSfile::SUCCESS;

  signed long numread = 0;
  unsigned char type = END;

  int emergencyExit = 255; // to avoid endless loop.
  signed long fieldLen; // <= 0 means end of file reached

  Clear();
  do {
    unsigned char *utf8 = nullptr;
    size_t utf8Len = 0;
    fieldLen = static_cast<signed long>(in->ReadField(type, utf8,
                                                      utf8Len));

    if (fieldLen > 0) {
      numread += fieldLen;
      if (IsItemDataField(type)) {
        if (!SetField(type, utf8, utf8Len)) {
          status = PWSfile::FAILURE;
          break;
        }
      } else if (IsItemAttField(type)) {
        // Allow rewind and retry
        if (utf8 != nullptr) {
          trashMemory(utf8, utf8Len * sizeof(utf8[0]));
          delete[] utf8;
        }
        return static_cast<int>(-numread);
      } else if (type != END) { // unknown field
        SetUnknownField(type, utf8Len, utf8);
      }
    } // if (fieldLen > 0)

    if (utf8 != nullptr) {
      trashMemory(utf8, utf8Len * sizeof(utf8[0]));
      delete[] utf8; utf8 = nullptr; utf8Len = 0;
    }
  } while (type != END && fieldLen > 0 && --emergencyExit > 0);

  if (numread > 0) {
    // Determine entry type:
    // ET_NORMAL (which may later change to ET_ALIASBASE or ET_SHORTCUTBASE)
    // ET_ALIAS or ET_SHORTCUT
    // For V4, this is simple, as we have different UUID types
    // For V3, we need to parse the password
    ParseSpecialPasswords();
    if (m_fields.find(UUID) != m_fields.end())
      m_entrytype = ET_NORMAL; // may change later to ET_*BASE
    else if (m_fields.find(ALIASUUID) != m_fields.end())
      m_entrytype = ET_ALIAS;
    else if (m_fields.find(SHORTCUTUUID) != m_fields.end())
      m_entrytype = ET_SHORTCUT;
    else 
      ASSERT(0);
    return status;
  } else
    return PWSfile::END_OF_FILE;
}

size_t CItemData::WriteIfSet(FieldType ft, PWSfile *out, bool isUTF8) const
{
  auto fiter = m_fields.find(ft);
  size_t retval = 0;
  if (fiter != m_fields.end()) {
    const CItemField &field = fiter->second;
    ASSERT(!field.IsEmpty());
    size_t flength = field.GetLength() + BlowFish::BLOCKSIZE;
    auto *pdata = new unsigned char[flength];
    CItem::GetField(field, pdata, flength);
    if (isUTF8) {
      wchar_t *wpdata = reinterpret_cast<wchar_t *>(pdata);
      size_t srclen = field.GetLength()/sizeof(TCHAR);
      wpdata[srclen] = 0;
      size_t dstlen = pws_os::wcstombs(nullptr, 0, wpdata, srclen);
      ASSERT(dstlen > 0);
      char *dst = new char[dstlen+1];
      dstlen = pws_os::wcstombs(dst, dstlen, wpdata, srclen);
      ASSERT(dstlen != size_t(-1));
      //[BR1150, BR1167]: Discard the terminating NULLs in text fields
      if (dstlen && !dst[dstlen-1])
        dstlen--;
      retval = out->WriteField(static_cast<unsigned char>(ft), reinterpret_cast<unsigned char *>(dst), dstlen);
      trashMemory(dst, dstlen);
      delete[] dst;
    } else {
      retval = out->WriteField(static_cast<unsigned char>(ft), pdata, field.GetLength());
    }
    trashMemory(pdata, flength);
    delete[] pdata;
  }
  return retval;
}

int CItemData::WriteCommon(PWSfile *out) const
{
  int i;

  const FieldType TextFields[] = {GROUP, TITLE, USER, PASSWORD, TWOFACTORKEY,
                                  NOTES, URL, AUTOTYPE, POLICY,
                                  PWHIST, RUNCMD, EMAIL,
                                  SYMBOLS, POLICYNAME,
                                  DATA_ATT_TITLE, DATA_ATT_MEDIATYPE, DATA_ATT_FILENAME,
                                  PASSKEY_RP_ID,
                                  END};
  const FieldType TimeFields[] = {ATIME, CTIME, XTIME, PMTIME, RMTIME, TOTPSTARTTIME,
                                  DATA_ATT_MTIME,
                                  END};
  const FieldType BinaryFields[] = { TOTPCONFIG, TOTPTIMESTEP, TOTPLENGTH, DATA_ATT_CONTENT,
                                  PASSKEY_CRED_ID, PASSKEY_USER_HANDLE, PASSKEY_ALGO_ID,
                                  PASSKEY_PRIVATE_KEY, PASSKEY_SIGN_COUNT,
                                  END };

  for (i = 0; TextFields[i] != END; i++)
    WriteIfSet(TextFields[i], out, true);

  for (i = 0; TimeFields[i] != END; i++) {
    time_t t = 0;
    CItem::GetTime(TimeFields[i], t);
    if (t != 0) {
      if (out->timeFieldLen() == 4) {
        unsigned char buf[4];
        putInt32(buf, static_cast<int32>(t));
        out->WriteField(static_cast<unsigned char>(TimeFields[i]), buf, out->timeFieldLen());
      } else if (out->timeFieldLen() == PWStime::TIME_LEN) {
        PWStime pwt(t);
        out->WriteField(static_cast<unsigned char>(TimeFields[i]), pwt, pwt.GetLength());
      } else ASSERT(0);
    } // t != 0
  }

  int32 i32 = 0;
  unsigned char buf32[sizeof(i32)];
  GetXTimeInt(i32);
  if (i32 > 0 && i32 <= 3650) {
    putInt(buf32, i32);
    out->WriteField(XTIME_INT, buf32, sizeof(int32));
  }

  i32 = 0;
  GetKBShortcut(i32);
  if (i32 != 0) {
    putInt(buf32, i32);
    out->WriteField(KBSHORTCUT, buf32, sizeof(int32));
  }

  int16 i16 = 0;
  unsigned char buf16[sizeof(i16)];
  GetDCA(i16);
  if (i16 >= PWSprefs::minDCA && i16 <= PWSprefs::maxDCA) {
    putInt(buf16, i16);
    out->WriteField(DCA, buf16, sizeof(int16));
  }
  i16 = 0;
  GetShiftDCA(i16);
  if (i16 >= PWSprefs::minDCA && i16 <= PWSprefs::maxDCA) {
    putInt(buf16, i16);
    out->WriteField(SHIFTDCA, buf16, sizeof(int16));
  }
  WriteIfSet(PROTECTED, out, false);

  for (i = 0; BinaryFields[i] != END; i++) {
    auto fiter = m_fields.find(BinaryFields[i]);
    if (fiter == m_fields.end())
      continue;
    const CItemField& field = fiter->second;
    ASSERT(!field.IsEmpty());
    std::vector<unsigned char> v;
    CItem::GetField(field, v);
    out->WriteField(static_cast<unsigned char>(BinaryFields[i]), &v[0], field.GetLength());
  }

  WriteUnknowns(out);
  // Assume that if previous write failed, last one will too.
  if (out->WriteField(END, _T("")) > 0) {
    return PWSfile::SUCCESS;
  } else {
    return PWSfile::FAILURE;
  }
}

int CItemData::Write(PWSfile *out) const
{
  // Map different UUID types (V4 concept) to original V3 UUID
  uuid_array_t item_uuid;
  FieldType ft = END;

  ASSERT(HasUUID());
  if (!IsDependent())
    ft = UUID;
  else if (IsAlias())
    ft = ALIASUUID;
  else if (IsShortcut())
    ft = SHORTCUTUUID;
  else ASSERT(0);
  GetUUID(item_uuid, ft);

  out->WriteField(UUID, item_uuid, sizeof(uuid_array_t));

  // We need to cast away constness to change Password field
  // for dependent entries
  // We restore the password afterwards (not that it should matter
  // for a dependent), so logically we're still const.

  auto *self = const_cast<CItemData *>(this);
  const StringX saved_password = GetPassword();
  self->SetSpecialPasswords(); // encode baseuuid in password if IsDependent

  int status = WriteCommon(out);

  self->SetPassword(saved_password);
  return status;
}

int CItemData::Write(PWSfileV4 *out) const
{
  uuid_array_t item_uuid;

  ASSERT(HasUUID());

  FieldType ft = END;

  ASSERT(HasUUID());
  if (!IsDependent())
    ft = UUID;
  else if (IsAlias())
    ft = ALIASUUID;
  else if (IsShortcut())
    ft = SHORTCUTUUID;
  else ASSERT(0);
  GetUUID(item_uuid, ft);

  out->WriteField(static_cast<unsigned char>(ft), item_uuid,
                  sizeof(uuid_array_t));
  if (IsDependent()) {
    uuid_array_t base_uuid;
    ASSERT(IsFieldSet(BASEUUID));
    GetUUID(base_uuid, BASEUUID);
    out->WriteField(BASEUUID, base_uuid, sizeof(uuid_array_t));
  }

  if (IsFieldSet(ATTREF)) {
    uuid_array_t ref_uuid;
    GetUUID(ref_uuid, ATTREF);
    out->WriteField(ATTREF, ref_uuid, sizeof(uuid_array_t));
  }

  int status = WriteCommon(out);

  return status;
}

int CItemData::WriteUnknowns(PWSfile *out) const
{
  for (auto uiter = m_URFL.begin();
       uiter != m_URFL.end();
       uiter++) {
    unsigned char type;
    size_t length = 0;
    unsigned char *pdata = nullptr;
    GetUnknownField(type, length, pdata, *uiter);
    out->WriteField(type, pdata, length);
    trashMemory(pdata, length);
    delete[] pdata;
  }
  return PWSfile::SUCCESS;
}

//-----------------------------------------------------------------------------
// Accessors

StringX CItemData::GetFieldValue(FieldType ft) const
{
  if (IsTextField(static_cast<unsigned char>(ft)) && ft != GROUPTITLE &&
      ft != NOTES && ft != PWHIST) {
    // Standard String fields
    return GetField(ft);
  } else {
    // Non-string fields or string fields that need special processing
    StringX str(_T(""));
    switch (ft) {
    case GROUPTITLE:  /* 0x00 */
      str = GetGroup() + TCHAR('.') + GetTitle();
      break;
    case UUID:        /* 0x01 */
    {
      uuid_array_t uuid_array = {0};
      GetUUID(uuid_array);
      str = CUUID(uuid_array, true);
      break;
    }
    case NOTES:        /* 0x05 */
      return GetNotes();
    case CTIME:        /* 0x07 */
      return GetCTimeL();
    case PMTIME:       /* 0x08 */
      return GetPMTimeL();
    case ATIME:        /* 0x09 */
      return GetATimeL();
    case XTIME:        /* 0x0a */
    {
      int32 xint(0);
      str = GetXTimeL();
      GetXTimeInt(xint);
      if (xint != 0)
        str += _T(" *");
      return str;
    }
    case RESERVED:     /* 0x0b */
      break;
    case RMTIME:       /* 0x0c */
      return GetRMTimeL();
    case PWHIST:       /* 0x0f */
      return GetPWHistory();
    case XTIME_INT:    /* 0x11 */
      return GetXTimeInt();
    case DCA:          /* 0x13 */
      return GetDCA();
    case PROTECTED:    /* 0x15 */
    {
      unsigned char uc;
      StringX sxProtected = _T("");
      GetProtected(uc);
      if (uc != 0)
        LoadAString(sxProtected, IDSC_YES);
      return sxProtected;
    }
    case SHIFTDCA:     /* 0x17 */
      return GetShiftDCA();
    case KBSHORTCUT:   /* 0x19 */
      return GetKBShortcut();
    case ATTREF:       /* 0x1a */
    case BASEUUID:     /* 0x41 */
    case ALIASUUID:    /* 0x42 */
    case SHORTCUTUUID: /* 0x43 */
    {
      uuid_array_t uuid_array = { 0 };
      GetUUID(uuid_array, ft);
      str = CUUID(uuid_array, true);
      break;
    }
    case TOTPCONFIG:
      str = GetTotpConfig();
      break;
    case TOTPTIMESTEP:
      str = GetTotpTimeStepSeconds();
      break;
    case TOTPLENGTH:
      str = GetTotpLength();
      break;
    case TOTPSTARTTIME:
      str = GetTotpStartTime();
      break;
    case DATA_ATT_MTIME:
      str = GetTime(DATA_ATT_MTIME, PWSUtil::TMC_LOCALE);
      break;
    case DATA_ATT_CONTENT:
      break;
    case PASSKEY_CRED_ID:
    {
      std::wostringstream oss;
      for (unsigned char c : GetPasskeyCredentialID())
        oss << std::hex << std::setw(2) << std::setfill(L'0') << static_cast<int>(c);
      str = oss.str().c_str();
      break;
    }
    case PASSKEY_USER_HANDLE:
    {
      std::wostringstream oss;
      for (unsigned char c : GetPasskeyUserHandle())
        oss << std::hex << std::setw(2) << std::setfill(L'0') << static_cast<int>(c);
      str = oss.str().c_str();
      break;
    }
    case PASSKEY_ALGO_ID:
      if (HasPasskey())
        str = std::to_wstring(GetPasskeyAlgorithmID()).c_str();
      break;
    case PASSKEY_PRIVATE_KEY:
      break; // never ever show to user
    case PASSKEY_SIGN_COUNT:
      if (HasPasskey())
        str = std::to_wstring(GetPasskeySignCount()).c_str();
      break;
    default:
      ASSERT(0);
    }
    return str;
  }
}

StringX CItemData::GetEffectiveFieldValue(FieldType ft, const CItemData *pbci) const
{
  if (IsNormal() || IsBase())
    return GetField(ft);

  // Here if we're a dependent;
  ASSERT(IsDependent());
  ASSERT(pbci != nullptr);

  if (IsAlias()) {
    std::vector<FieldType> base_fields = {
      PASSWORD,
      PWHIST,
      TWOFACTORKEY,
      TOTPCONFIG,
      TOTPSTARTTIME,
      TOTPTIMESTEP,
      TOTPLENGTH
    };
    // Only base_fields fields (i.e., current password and history, TOTP parameters)
    // are taken from base entry. Everything else is from the actual entry.
    if (std::find(base_fields.begin(), base_fields.end(), ft) != base_fields.end())
      return pbci->GetField(ft);
    else
      return GetField(ft);
  } else if (IsShortcut()) {
    // For a shortcut everything is taken from its base entry,
    // except the group, title and user.
    if (ft == GROUP || ft == TITLE || ft == USER)
      return GetField(ft);
    else
      return pbci->GetField(ft);
  } else {
    ASSERT(0);
    return _T("");
  }
}

static void CleanNotes(StringX &s, TCHAR delimiter)
{
  if (delimiter != 0) {
    StringX r2;
    for (StringX::iterator iter = s.begin(); iter != s.end(); iter++)
      switch (*iter) {
      case TCHAR('\r'): continue;
      case TCHAR('\n'): r2 += delimiter; continue;
      default: r2 += *iter;
      }
    s = r2;
  }
}

StringX CItemData::GetNotes(TCHAR delimiter) const
{
  StringX ret = GetField(NOTES);
  CleanNotes(ret, delimiter);
  return ret;
}

StringX CItemData::GetTime(int whichtime, PWSUtil::TMC result_format, bool convert_epoch, bool utc_time) const
{
  time_t t;

  CItem::GetTime(whichtime, t);
  return PWSUtil::ConvertToDateTimeString(t, result_format, convert_epoch, utc_time);
}

void CItemData::GetUUID(uuid_array_t &uuid_array, FieldType ft) const
{
  size_t length = sizeof(uuid_array_t);
  auto fiter = m_fields.end();
  if (ft != END) { // END means "infer correct UUID from entry type"
    // anything != END is used as-is, no questions asked
    fiter = m_fields.find(ft);
  } else switch (m_entrytype) {
    case ET_NORMAL:
    case ET_ALIASBASE:
    case ET_SHORTCUTBASE:
      fiter = m_fields.find(UUID);
      break;
    case ET_ALIAS:
      fiter = m_fields.find(ALIASUUID);
      break;
    case ET_SHORTCUT:
      fiter = m_fields.find(SHORTCUTUUID);
      break;
    default:
      ASSERT(0);
    }
  if (fiter == m_fields.end()) {
    // pws_os::Trace(_T("CItemData::GetUUID(uuid_array_t) - no UUID found!\n"));
    memset(uuid_array, 0, length);
  } else {
    CItem::GetField(fiter->second,
                    static_cast<unsigned char *>(uuid_array), length);
  }
}

const CUUID CItemData::GetUUID(FieldType ft) const
{
  // Ideally we'd like to return a uuid_array_t, but C++ doesn't
  // allow array return values.
  // If we returned the uuid_array_t pointer, we'd have a scope problem,
  // as the pointer's owner would be deleted too soon.
  // Frustrating, but that's life...

  uuid_array_t ua;
  GetUUID(ua, ft);
  return CUUID(ua);
}

void CItemData::GetPWPolicy(PWPolicy &pwp) const
{
  PWPolicy mypol(GetField(POLICY));
  pwp = mypol;
}

int32 CItemData::GetXTimeInt(int32 &xint) const
{
  auto fiter = m_fields.find(XTIME_INT);
  if (fiter == m_fields.end())
    xint = 0;
  else {
    unsigned char in[TwoFish::BLOCKSIZE]; // required by GetField
    size_t tlen = sizeof(in); // ditto

    CItem::GetField(fiter->second, in, tlen);
    if (tlen != 0) {
      ASSERT(tlen == sizeof(int32));
      xint = getInt<int32>(in);
    } else {
      xint = 0;
    }
  }
  return xint;
}

StringX CItemData::GetXTimeInt() const
{
  int32 xint;
  GetXTimeInt(xint);
  if (xint == 0)
    return _T("");

  oStringXStream os;
  os << xint;
  return os.str();
}

void CItemData::GetProtected(unsigned char &ucprotected) const
{
  auto fiter = m_fields.find(PROTECTED);
  if (fiter == m_fields.end())
    ucprotected = 0;
  else {
    unsigned char in[TwoFish::BLOCKSIZE]; // required by GetField
    size_t tlen = sizeof(in); // ditto
    CItem::GetField(fiter->second, in, tlen);
    if (tlen != 0) {
      ASSERT(tlen == sizeof(char));
      ucprotected = in[0];
    } else {
      ucprotected = 0;
    }
  }
}

bool CItemData::IsProtected() const
{
  unsigned char ucprotected;
  GetProtected(ucprotected);
  return ucprotected != 0;
}

StringX CItemData::GetProtected() const
{
  return IsProtected() ? StringX(_T("1")) : StringX(_T(""));
}

bool CItemData::IsDCASet(bool bShift) const
{
    auto fiter = m_fields.find(bShift ? SHIFTDCA : DCA);
    return (fiter != m_fields.end());
}

int16 CItemData::GetDCA(int16 &iDCA, const bool bShift) const
{
  auto fiter = m_fields.find(bShift ? SHIFTDCA : DCA);
  if (fiter != m_fields.end()) {
    unsigned char in[TwoFish::BLOCKSIZE]; // required by GetField
    size_t tlen = sizeof(in); // ditto
    CItem::GetField(fiter->second, in, tlen);

    if (tlen != 0) {
      ASSERT(tlen == sizeof(int16));
      iDCA = getInt<int16>(in);
    } else {
      iDCA = -1;
    }
  } else // fiter == m_fields.end()
    iDCA = -1;
  return iDCA;
}

StringX CItemData::GetDCA(const bool bShift) const
{
  int16 dca;
  GetDCA(dca, bShift);
  oStringXStream os;
  os << dca;
  return os.str();
}

int32 CItemData::GetKBShortcut(int32 &iKBShortcut) const
{
  auto fiter = m_fields.find(KBSHORTCUT);
  if (fiter != m_fields.end()) {
    unsigned char in[TwoFish::BLOCKSIZE]; // required by GetField
    size_t tlen = sizeof(in); // ditto
    CItem::GetField(fiter->second, in, tlen);

    if (tlen != 0) {
      ASSERT(tlen == sizeof(int32));
      iKBShortcut = getInt<int32>(in);
    } else {
      iKBShortcut = 0;
    }
  } else // fiter == m_fields.end()
    iKBShortcut = 0;
  return iKBShortcut;
}

StringX CItemData::GetKBShortcut() const
{
  int32 iKBShortcut;
  GetKBShortcut(iKBShortcut);

  if (iKBShortcut != 0) {
    StringX kbs(_T(""));

    WORD wVirtualKeyCode = iKBShortcut & 0xff;
    WORD wPWSModifiers = iKBShortcut >> 16;

    if (iKBShortcut != 0) {
      if (wPWSModifiers & PWS_HOTKEYF_ALT)
        kbs += _T("A");
      if (wPWSModifiers & PWS_HOTKEYF_CONTROL)
        kbs += _T("C");
      if (wPWSModifiers & PWS_HOTKEYF_SHIFT)
        kbs += _T("S");
      if (wPWSModifiers & PWS_HOTKEYF_EXT)
        kbs += _T("E");
      if (wPWSModifiers & PWS_HOTKEYF_META)
        kbs += _T("M");
      if (wPWSModifiers & PWS_HOTKEYF_WIN)
        kbs += _T("W");
      if (wPWSModifiers & PWS_HOTKEYF_CMD)
        kbs += _T("D");

      kbs += _T(":");
      ostringstreamT os1;
      os1 << hex << setfill(charT('0')) << setw(4) << wVirtualKeyCode;
      kbs += os1.str().c_str();
      return kbs;
    }
  }
  return _T("");
}

StringX CItemData::GetPWHistory() const
{
  StringX ret = GetField(PWHIST);
  if (ret == _T("0") || ret == _T("00000"))
    ret = _T("");
  return ret;
}

StringX CItemData::GetPreviousPassword() const
{
  return PWHistList::GetPreviousPassword(GetField(PWHIST));
}

StringX CItemData::GetPlaintext(const TCHAR &separator,
                                const FieldBits &bsFields,
                                const TCHAR &delimiter,
                                const CItemData *pcibase) const
{
  StringX ret(_T(""));

  StringX grouptitle;
  const StringX title(GetTitle());
  const StringX group(GetGroup());
  const StringX user(GetUser());
  const StringX url(GetURL());
  const StringX notes(GetNotes(delimiter));

  // a '.' in title gets Import confused re: Groups
  grouptitle = title;
  if (grouptitle.find(TCHAR('.')) != StringX::npos) {
    if (delimiter != 0) {
      StringX s;
      for (StringX::iterator iter = grouptitle.begin();
           iter != grouptitle.end(); iter++)
        s += (*iter == TCHAR('.')) ? delimiter : *iter;
      grouptitle = s;
    } else {
      grouptitle = TCHAR('\"') + title + TCHAR('\"');
    }
  }

  if (!group.empty())
    grouptitle = group + TCHAR('.') + grouptitle;

  StringX history(_T(""));
  if (bsFields.test(CItemData::PWHIST)) {
    // History exported as "00000" if empty, to make parsing easier
    PWHistList pwhistlist(GetPWHistory(), PWSUtil::TMC_EXPORT_IMPORT);

    //  Build export string
    history = pwhistlist.MakePWHistoryHeader();
    PWHistList::iterator iter;
    for (iter = pwhistlist.begin(); iter != pwhistlist.end(); iter++) {
      const PWHistEntry &pwshe = *iter;
      history += _T(' ');
      history += pwshe.changedate;
      ostringstreamT os1;
      os1 << hex << charT(' ') << setfill(charT('0')) << setw(4)
          << pwshe.password.length() << charT(' ');
      history += os1.str().c_str();
      history += pwshe.password;
    }
  }

  StringX csPassword = ResolvePlaceholderEligibleField(this, pcibase, [this] { return GetPassword(); });

  StringX csTwoFactorKey;
  StringX csTotpConfig;
  StringX csTotpStartTime;
  StringX csTotpTimeStep;
  StringX csTotpLength;
  if (IsTotpActive()) {
    csTwoFactorKey = GetTwoFactorKey();
    if (!IsTotpConfigDefault())
      csTotpConfig = GetTotpConfig();
    if (!IsTotpStartTimeDefault())
      csTotpStartTime = GetTotpStartTime();
    if (!IsTotpTimeStepSecondsDefault())
      csTotpTimeStep = GetTotpTimeStepSeconds();
    if (!IsTotpLengthDefault())
      csTotpLength = GetTotpLength();
  }

  // Notes field must be last, for ease of parsing import
  if (bsFields.count() == bsFields.size()) {
    // Everything - note can't actually set all bits via dialog!
    // Must be in same order as full header
    unsigned char uc;
    GetProtected(uc);
    StringX sxProtected = uc != 0 ? _T("Y") : _T("N");
    ret = (grouptitle + separator +
           user + separator +
           csPassword + separator +
           csTwoFactorKey + separator +
           csTotpConfig + separator +
           csTotpStartTime + separator +
           csTotpTimeStep + separator +
           csTotpLength + separator +
           url + separator +
           GetAutoType() + separator +
           GetCTimeExp() + separator +
           GetPMTimeExp() + separator +
           GetATimeExp() + separator +
           GetXTimeExp() + separator +
           GetXTimeInt() + separator +
           GetRMTimeExp() + separator +
           GetPWPolicy() + separator +
           GetPolicyName() + separator +
           history + separator +
           GetRunCommand() + separator +
           GetDCA() + separator +
           GetShiftDCA() + separator +
           GetEmail() + separator +
           sxProtected + separator +
           GetSymbols() + separator +
           GetKBShortcut() + separator +
           _T("\"") + notes + _T("\""));
  } else {
    // Not everything
    // Must be in same order as custom header
    if (bsFields.test(CItemData::GROUP) && bsFields.test(CItemData::TITLE))
      ret += grouptitle + separator;
    else if (bsFields.test(CItemData::GROUP))
      ret += group + separator;
    else if (bsFields.test(CItemData::TITLE))
      ret += title + separator;
    if (bsFields.test(CItemData::USER))
      ret += user + separator;
    if (bsFields.test(CItemData::PASSWORD))
      ret += csPassword + separator;
    if (bsFields.test(CItemData::TWOFACTORKEY))
      ret += csTwoFactorKey + separator;
    if (bsFields.test(CItemData::TOTPCONFIG))
      ret += csTotpConfig + separator;
    if (bsFields.test(CItemData::TOTPSTARTTIME))
      ret += csTotpStartTime + separator;
    if (bsFields.test(CItemData::TOTPTIMESTEP))
      ret += csTotpTimeStep + separator;
    if (bsFields.test(CItemData::TOTPLENGTH))
      ret += csTotpLength + separator;
    if (bsFields.test(CItemData::URL))
      ret += url + separator;
    if (bsFields.test(CItemData::AUTOTYPE))
      ret += GetAutoType() + separator;
    if (bsFields.test(CItemData::CTIME))
      ret += GetCTimeExp() + separator;
    if (bsFields.test(CItemData::PMTIME))
      ret += GetPMTimeExp() + separator;
    if (bsFields.test(CItemData::ATIME))
      ret += GetATimeExp() + separator;
    if (bsFields.test(CItemData::XTIME))
      ret += GetXTimeExp() + separator;
    if (bsFields.test(CItemData::XTIME_INT))
      ret += GetXTimeInt() + separator;
    if (bsFields.test(CItemData::RMTIME))
      ret += GetRMTimeExp() + separator;

    StringX sxPolicyName = GetPolicyName();
    if (sxPolicyName.empty()) {
      // print policy only if policy name is not available
      if (bsFields.test(CItemData::POLICY))
        ret += GetPWPolicy() + separator;
      if (bsFields.test(CItemData::POLICYNAME))
        ret += separator;
    } else {
      // if policy name is available, ignore the policy
      if (bsFields.test(CItemData::POLICY))
        ret += separator;
      if (bsFields.test(CItemData::POLICYNAME))
        ret += sxPolicyName + separator;
    }

    if (bsFields.test(CItemData::PWHIST))
      ret += history + separator;
    if (bsFields.test(CItemData::RUNCMD))
      ret += GetRunCommand() + separator;
    if (bsFields.test(CItemData::DCA))
      ret += GetDCA() + separator;
    if (bsFields.test(CItemData::SHIFTDCA))
      ret += GetShiftDCA() + separator;
    if (bsFields.test(CItemData::EMAIL))
      ret += GetEmail() + separator;
    if (bsFields.test(CItemData::PROTECTED)) {
      unsigned char uc;
      GetProtected(uc);
      StringX sxProtected = uc != 0 ? _T("Y") : _T("N");
      ret += sxProtected + separator;
    }
    if (bsFields.test(CItemData::SYMBOLS))
      ret += GetSymbols() + separator;

    if (bsFields.test(CItemData::KBSHORTCUT)) {
      ret += GetKBShortcut() + separator;
    }

    if (bsFields.test(CItemData::NOTES))
      ret += _T("\"") + notes + _T("\"");
    // remove trailing separator
    if (ret[ret.length()-1] == separator) {
      size_t rl = ret.length();
      ret = ret.substr(0, rl - 1);
    }
  }

  return ret;
}

static void ConditionalWriteXML(int field, const CItemData::FieldBits &fieldbits,
                                const char *name, const StringX value,
                                ostringstream &oss, CUTF8Conv &utf8conv, bool &errors)
{
  if (fieldbits.test(field) && !value.empty()) {
    if (!PWSUtil::WriteXMLField(oss, name, value, utf8conv))
      errors = true;
  }
}

string CItemData::GetXML(unsigned id, const FieldBits &bsExport,
                         TCHAR delimiter, const CItemData *pcibase,
                         bool bforce_normal_entry,
                         bool &bXMLErrorsFound) const
{
  bXMLErrorsFound = false;
  ostringstream oss; // ALWAYS a string of chars, never wchar_t!
  oss << "\t<entry id=\"" << dec << id << "\"";
  if (bforce_normal_entry)
    oss << " normal=\"true\"";

  oss << ">" << endl;

  StringX tmp;
  CUTF8Conv utf8conv;
  unsigned char uc;
  bool brc;

  ConditionalWriteXML(CItemData::GROUP, bsExport, "group", GetGroup(),
                      oss, utf8conv, bXMLErrorsFound);

  // Title mandatory (see pwsafe.xsd)
  brc = PWSUtil::WriteXMLField(oss, "title", GetTitle(), utf8conv);
  if (!brc) bXMLErrorsFound = true;

  ConditionalWriteXML(CItemData::USER, bsExport, "username", GetUser(),
                      oss, utf8conv, bXMLErrorsFound);

  tmp = ResolvePlaceholderEligibleField(this, pcibase, [this] { return GetPassword(); });
  brc = PWSUtil::WriteXMLField(oss, "password", tmp, utf8conv);
  if (!brc) bXMLErrorsFound = true;

  if (IsTotpActive()) {
    ConditionalWriteXML(CItemData::TWOFACTORKEY, bsExport, GetXmlFieldName(TWOFACTORKEY).c_str(), GetTwoFactorKey(),
                        oss, utf8conv, bXMLErrorsFound);

    if (!IsTotpConfigDefault()) {
      ConditionalWriteXML(CItemData::TWOFACTORKEY, bsExport, GetXmlFieldName(TOTPCONFIG).c_str(), GetTotpConfig(),
                        oss, utf8conv, bXMLErrorsFound);
    }

    if (!IsTotpStartTimeDefault() && bsExport.test(CItemData::TOTPSTARTTIME)) {
      oss << PWSUtil::GetXMLTime(2, GetXmlFieldName(TOTPSTARTTIME).c_str(), GetTotpStartTimeAsTimeT(), utf8conv, true, true);
    }

    if (!IsTotpTimeStepSecondsDefault() && bsExport.test(CItemData::TOTPTIMESTEP)) {
      brc = PWSUtil::WriteXMLField(oss, GetXmlFieldName(TOTPTIMESTEP).c_str(), GetTotpTimeStepSeconds(), utf8conv);
      if (!brc) bXMLErrorsFound = true;
    }

    if (!IsTotpLengthDefault() && bsExport.test(CItemData::TOTPLENGTH)) {
      brc = PWSUtil::WriteXMLField(oss, GetXmlFieldName(TOTPLENGTH).c_str(), GetTotpLength(), utf8conv);
      if (!brc) bXMLErrorsFound = true;
    }
  }

  ConditionalWriteXML(CItemData::URL, bsExport, "url", GetURL(),
                      oss, utf8conv, bXMLErrorsFound);
  ConditionalWriteXML(CItemData::AUTOTYPE, bsExport, "autotype", GetAutoType(),
                      oss, utf8conv, bXMLErrorsFound);

  tmp = GetNotes();
  if (bsExport.test(CItemData::NOTES) && !tmp.empty()) {
    CleanNotes(tmp, delimiter);
    brc = PWSUtil::WriteXMLField(oss, "notes", tmp, utf8conv);
    if (!brc) bXMLErrorsFound = true;
  }

  oss << "\t\t<uuid><![CDATA[" << GetUUID() << "]]></uuid>" << endl;

  time_t t;
  int32 i32;
  int16 i16;

  GetCTime(t);
  if (bsExport.test(CItemData::CTIME) && t)
    oss << PWSUtil::GetXMLTime(2, "ctimex", t, utf8conv);

  GetATime(t);
  if (bsExport.test(CItemData::ATIME) && t)
    oss << PWSUtil::GetXMLTime(2, "atimex", t, utf8conv);

  GetXTime(t);
  if (bsExport.test(CItemData::XTIME) && t)
    oss << PWSUtil::GetXMLTime(2, "xtimex", t, utf8conv);

  GetXTimeInt(i32);
  if (bsExport.test(CItemData::XTIME_INT) && i32 > 0 && i32 <= 3650)
    oss << "\t\t<xtime_interval>" << dec << i32 << "</xtime_interval>" << endl;

  GetPMTime(t);
  if (bsExport.test(CItemData::PMTIME) && t)
    oss << PWSUtil::GetXMLTime(2, "pmtimex", t, utf8conv);

  GetRMTime(t);
  if (bsExport.test(CItemData::RMTIME) && t)
    oss << PWSUtil::GetXMLTime(2, "rmtimex", t, utf8conv);

  StringX sxPolicyName = GetPolicyName();
  if (sxPolicyName.empty()) {
    PWPolicy pwp;
    GetPWPolicy(pwp);
    if (bsExport.test(CItemData::POLICY) && pwp.flags != 0) {
      oss << "\t\t<PasswordPolicy>" << endl;
      oss << dec;
      oss << "\t\t\t<PWLength>" << pwp.length << "</PWLength>" << endl;
      if (pwp.flags & PWPolicy::UseLowercase)
        oss << "\t\t\t<PWUseLowercase>1</PWUseLowercase>" << endl;
      if (pwp.flags & PWPolicy::UseUppercase)
        oss << "\t\t\t<PWUseUppercase>1</PWUseUppercase>" << endl;
      if (pwp.flags & PWPolicy::UseDigits)
        oss << "\t\t\t<PWUseDigits>1</PWUseDigits>" << endl;
      if (pwp.flags & PWPolicy::UseSymbols)
        oss << "\t\t\t<PWUseSymbols>1</PWUseSymbols>" << endl;
      if (pwp.flags & PWPolicy::UseHexDigits)
        oss << "\t\t\t<PWUseHexDigits>1</PWUseHexDigits>" << endl;
      if (pwp.flags & PWPolicy::UseEasyVision)
        oss << "\t\t\t<PWUseEasyVision>1</PWUseEasyVision>" << endl;
      if (pwp.flags & PWPolicy::MakePronounceable)
        oss << "\t\t\t<PWMakePronounceable>1</PWMakePronounceable>" << endl;

      if (pwp.lowerminlength > 0) {
        oss << "\t\t\t<PWLowercaseMinLength>" << pwp.lowerminlength << "</PWLowercaseMinLength>" << endl;
      }
      if (pwp.upperminlength > 0) {
        oss << "\t\t\t<PWUppercaseMinLength>" << pwp.upperminlength << "</PWUppercaseMinLength>" << endl;
      }
      if (pwp.digitminlength > 0) {
        oss << "\t\t\t<PWDigitMinLength>" << pwp.digitminlength << "</PWDigitMinLength>" << endl;
      }
      if (pwp.symbolminlength > 0) {
        oss << "\t\t\t<PWSymbolMinLength>" << pwp.symbolminlength << "</PWSymbolMinLength>" << endl;
      }
      oss << "\t\t</PasswordPolicy>" << endl;
    }
  } else {
    if (bsExport.test(CItemData::POLICY) || bsExport.test(CItemData::POLICYNAME)) {
      brc = PWSUtil::WriteXMLField(oss, "PasswordPolicyName", sxPolicyName,
                        utf8conv, "\t\t");
      if (!brc) bXMLErrorsFound = true;
    }
  }

  if (bsExport.test(CItemData::PWHIST)) {
    PWHistList pwhistlist(GetPWHistory(), PWSUtil::TMC_XML);
    bool pwh_status = pwhistlist.isSaving();
    size_t pwh_max = pwhistlist.getMax();

    oss << dec;
    if (pwh_status || pwh_max > 0 || !pwhistlist.empty()) {
      oss << "\t\t<pwhistory>" << endl;
      oss << "\t\t\t<status>" << pwh_status << "</status>" << endl;
      oss << "\t\t\t<max>" << pwh_max << "</max>" << endl;
      oss << "\t\t\t<num>" << pwhistlist.size() << "</num>" << endl;
      if (!pwhistlist.empty()) {
        oss << "\t\t\t<history_entries>" << endl;
        int num = 1;
        PWHistList::iterator hiter;
        for (hiter = pwhistlist.begin(); hiter != pwhistlist.end();
             hiter++) {
          const unsigned char *utf8 = nullptr;
          size_t utf8Len = 0;

          oss << "\t\t\t\t<history_entry num=\"" << num << "\">" << endl;
          const PWHistEntry &pwshe = *hiter;
          oss << "\t\t\t\t\t<changedx>";
          if (utf8conv.ToUTF8(pwshe.changedate.substr(0, 10), utf8, utf8Len))
            oss.write(reinterpret_cast<const char *>(utf8), utf8Len);
          else
            oss << "1970-01-01";

          oss << "T";
          if (utf8conv.ToUTF8(pwshe.changedate.substr(pwshe.changedate.length() - 8),
                              utf8, utf8Len))
            oss.write(reinterpret_cast<const char *>(utf8), utf8Len);
          else
            oss << "00:00";

          oss << "</changedx>" << endl;
          brc = PWSUtil::WriteXMLField(oss, "oldpassword", pwshe.password,
                        utf8conv, "\t\t\t\t\t");
          if (!brc) bXMLErrorsFound = true;

          oss << "\t\t\t\t</history_entry>" << endl;

          num++;
        } // for
        oss << "\t\t\t</history_entries>" << endl;
      } // if !empty
      oss << "\t\t</pwhistory>" << endl;
    }
  }

  ConditionalWriteXML(CItemData::RUNCMD, bsExport, "runcommand", GetRunCommand(),
                      oss, utf8conv, bXMLErrorsFound);

  GetDCA(i16);
  if (bsExport.test(CItemData::DCA) &&
      i16 >= PWSprefs::minDCA && i16 <= PWSprefs::maxDCA)
    oss << "\t\t<dca>" << i16 << "</dca>" << endl;

  GetShiftDCA(i16);
  if (bsExport.test(CItemData::SHIFTDCA) &&
      i16 >= PWSprefs::minDCA && i16 <= PWSprefs::maxDCA)
    oss << "\t\t<shiftdca>" << i16 << "</shiftdca>" << endl;

  ConditionalWriteXML(CItemData::EMAIL, bsExport, "email", GetEmail(),
                      oss, utf8conv, bXMLErrorsFound);

  GetProtected(uc);
  if (bsExport.test(CItemData::PROTECTED) && uc != 0)
    oss << "\t\t<protected>1</protected>" << endl;

  ConditionalWriteXML(CItemData::SYMBOLS, bsExport, "symbols", GetSymbols(),
                      oss, utf8conv, bXMLErrorsFound);
  ConditionalWriteXML(CItemData::KBSHORTCUT, bsExport, "kbshortcut", GetKBShortcut(),
                      oss, utf8conv, bXMLErrorsFound);

  oss << "\t</entry>" << endl << endl;
  return oss.str();
}

void CItemData::SplitName(const StringX &name,
                          StringX &title, StringX &username)
{
  StringX::size_type pos = name.find(SPLTCHR);
  if (pos == StringX::npos) {//Not a split name
    StringX::size_type pos2 = name.find(DEFUSERCHR);
    if (pos2 == StringX::npos)  {//Make certain that you remove the DEFUSERCHR
      title = name;
    } else {
      title = (name.substr(0, pos2));
    }
  } else {
    /*
    * There should never ever be both a SPLTCHR and a DEFUSERCHR in
    * the same string
    */
    StringX temp;
    temp = name.substr(0, pos);
    TrimRight(temp);
    title = temp;
    temp = name.substr(pos+1); // Zero-index string
    TrimLeft(temp);
    username = temp;
  }
}

//-----------------------------------------------------------------------------
// Setters

void CItemData::CreateUUID(FieldType ft)
{
  CUUID uuid;
  if (ft == END) {
    switch (m_entrytype) {
    case ET_NORMAL: case ET_SHORTCUTBASE: case ET_ALIASBASE:
      ft = UUID; break;
    case ET_ALIAS: ft = ALIASUUID; break;
    case ET_SHORTCUT: ft = SHORTCUTUUID; break;
    default: ASSERT(0); ft = UUID; break;
    }
  }
  SetUUID(uuid, ft);
}

void CItemData::SetName(const StringX &name, const StringX &defaultUsername)
{
  // the m_name is from pre-2.0 versions, and may contain the title and user
  // separated by SPLTCHR. Also, DEFUSERCHR signified that the default username is to be used.
  // Here we fill the title and user fields so that
  // the application can ignore this difference after an ItemData record
  // has been created
  StringX title, user;
  StringX::size_type pos = name.find(DEFUSERCHR);
  if (pos != StringX::npos) {
    title = name.substr(0, pos);
    user = defaultUsername;
  } else
    SplitName(name, title, user);
  CItem::SetField(NAME, name);
  CItem::SetField(TITLE, title);
  CItem::SetField(USER, user);
}

void CItemData::SetTitle(const StringX &title, TCHAR delimiter)
{
  if (delimiter == 0)
    CItem::SetField(TITLE, title);
  else {
    StringX new_title(_T(""));
    StringX newstringT, tmpstringT;
    StringX::size_type pos = 0;

    newstringT = title;
    do {
      pos = newstringT.find(delimiter);
      if ( pos != StringX::npos ) {
        new_title += newstringT.substr(0, pos) + _T(".");

        tmpstringT = newstringT.substr(pos + 1);
        newstringT = tmpstringT;
      }
    } while ( pos != StringX::npos );

    if (!newstringT.empty())
      new_title += newstringT;

    CItem::SetField(TITLE, new_title);
  }
}

void CItemData::UpdatePassword(const StringX &password)
{
  // use when password changed - manages history, modification times
  UpdatePasswordHistory();
  SetPassword(password);

  time_t t;
  time(&t);
  SetPMTime(t);

  int32 xint;
  GetXTimeInt(xint);
  if (xint != 0) {
    // convert days to seconds for time_t
    t += (xint * 86400);
    SetXTime(t);
  } else {
    SetXTime(time_t(0));
  }
}

void CItemData::UpdatePasswordHistory()
{
  const StringX pwh_str = GetPWHistory();
  PWHistList pwhistlist(pwh_str, PWSUtil::TMC_EXPORT_IMPORT);

  if (pwh_str.empty()) {
    // If GetPWHistory() is empty, use preference values!
    const PWSprefs *prefs = PWSprefs::GetInstance();
    pwhistlist.setSaving(prefs->GetPref(PWSprefs::SavePasswordHistory));
    pwhistlist.setMax(prefs->GetPref(PWSprefs::NumPWHistoryDefault));
  }
    
  if (!pwhistlist.isSaving())
    return;

  time_t t;
  GetPMTime(t); // get mod time of last password

  if (!t) // if never set - try creation date
    GetCTime(t);

  PWHistEntry pwh_ent;
  pwh_ent.password = GetPassword();
  pwh_ent.changetttdate = t;
  pwh_ent.changedate =
    PWSUtil::ConvertToDateTimeString(t, PWSUtil::TMC_EXPORT_IMPORT);

  if (pwh_ent.changedate.empty()) {
    StringX unk;
    LoadAString(unk, IDSC_UNKNOWN);
    pwh_ent.changedate = unk;
  }

  // Now add the latest PW to the history list
  pwhistlist.addEntry(pwh_ent);

  // Remove the excess and format as a StringX
  StringX new_PWHistory = pwhistlist;
  SetPWHistory(new_PWHistory);
}

void CItemData::SetNotes(const StringX &notes, TCHAR delimiter)
{
  if (delimiter == 0)
    CItem::SetField(NOTES, notes);
  else {
    const StringX CRLF = _T("\r\n");
    StringX multiline_notes(_T(""));

    StringX newstringT;
    StringX tmpstringT;

    StringX::size_type pos = 0;

    newstringT = notes;
    do {
      pos = newstringT.find(delimiter);
      if ( pos != StringX::npos ) {
        multiline_notes += newstringT.substr(0, pos) + CRLF;

        tmpstringT = newstringT.substr(pos + 1);
        newstringT = tmpstringT;
      }
    } while ( pos != StringX::npos );

    if (!newstringT.empty())
      multiline_notes += newstringT;

    CItem::SetField(NOTES, multiline_notes);
  }
}

void CItemData::SetUUID(const CUUID &uuid, FieldType ft)
{
  CItem::SetField(ft, static_cast<const unsigned char *>(*uuid.GetARep()), sizeof(uuid_array_t));
}

void CItemData::SetTime(int whichtime)
{
  time_t t;
  time(&t);
  CItem::SetTime(whichtime, t);
}

// CItemData::SetTime sets a field's time given a time string which can be
// interpreted as follows...
//  * If time_str == "" then set the field's time to 0 time_t.
//  * If time_str == "now" then set the field's time to current UTC time_t.
//  * If neither of the above cases match, try to parse a time stamp in
//    order of the following functions:
//      - VerifyImportDateTimeString
//      - VerifyXMLDateTimeString
//      - VerifyASCDateTimeString
//    Each of the 3 functions, if successful in parsing the time stamp in time_str,
//    interprets the time_str as a local time stamp value, returning a time_t in UTC
//    time representing that local time. For example, if time_str is "1970/01/02 00:00:00"
//    and the timezone is PDT (GMT-8), the field's value will be set to the time_t value
//    plus 8 hours. For cases where the incoming time_str should be interpreted as GMT
//    time, the utc_time argument can be set to true.
bool CItemData::SetTime(int whichtime, const stringT &time_str, bool utc_time)
{
  time_t t(0);

  if (time_str.empty()) {
    CItem::SetTime(whichtime, t);
    return true;
  } else
    if (time_str == _T("now")) {
      time(&t);
      CItem::SetTime(whichtime, t);
      return true;
    } else
      if ((VerifyImportDateTimeString(time_str, t, utc_time) ||
           VerifyXMLDateTimeString(time_str, t, utc_time) ||
           VerifyASCDateTimeString(time_str, t, utc_time)) &&
          (t != time_t(-1))  // checkerror despite all our verification!
          ) {
        CItem::SetTime(whichtime, t);
        return true;
      }
  return false;
}

void CItemData::SetDuplicateTimes(const CItemData &src)
{
  // As per FR819
  // Note: potential date/time inconsistencies that should not be "fixed"
  // during open validation i.e. fields changed before the entry was created!

  // Set creation time to now but keep all others unchanged.
  // (ignore last access time as will be updated if the user has requested
  // that these are maintained).
  SetCTime();

  time_t original_creation_time, t;
  original_creation_time = src.GetCTime(original_creation_time);

  // If the password & entry modification times are zero, they haven't
  // been changed since the entry was created.  Use original creation times.
  if (!src.IsShortcut()) {
    // Shortcuts don't have a password that a user can change
    t = src.GetPMTime(t);
    SetPMTime(t == 0 ? original_creation_time : t);
  }

  // Set record modification time
  t = src.GetRMTime(t);
  SetRMTime(t == 0 ? original_creation_time : t);
}

void CItemData::SetXTimeInt(int32 xint)
{
  unsigned char buf[sizeof(int32)];
  putInt(buf, xint);
  CItem::SetField(XTIME_INT, buf, sizeof(int32));
}

bool CItemData::SetXTimeInt(const stringT &xint_str)
{
  int32 xint(0);

  if (xint_str.empty()) {
    SetXTimeInt(xint);
    return true;
  }

  if (xint_str.find_first_not_of(_T("0123456789")) == stringT::npos) {
    istringstreamT is(xint_str);
    is >> xint;
    if (is.fail())
      return false;
    if (xint >= 0 && xint <= 3650) {
      SetXTimeInt(xint);
      return true;
    }
  }
  return false;
}

bool CItemData::SetFieldAsByte(CItem::FieldType type, const stringT& byte_str, bool strict)
{
  if (byte_str.empty()) {
    uint8_t zero = 0;
    SetField(type, &zero, 1);
    return true;
  }

  if (byte_str.find_first_not_of(_T("0123456789")) != stringT::npos)
    return false;

  istringstreamT is(byte_str);
  int v;
  is >> v;
  if (is.fail())
    return false;
  if (strict && (v < 0 || v > 255))
    return false;
  uint8_t byte_value = static_cast<uint8_t>(v);
  SetField(type, &byte_value, 1);
  return true;
}

void CItemData::SetPWHistory(const StringX &PWHistory)
{
  StringX pwh = PWHistory;
  if (pwh == _T("0") || pwh == _T("00000"))
    pwh = _T("");
  CItem::SetField(PWHIST, pwh);
}

void CItemData::SetPWPolicy(const PWPolicy &pwp)
{
  const StringX cs_pwp(pwp);

  CItem::SetField(POLICY, cs_pwp);
  if (!pwp.symbols.empty())
    SetSymbols(pwp.symbols);
}

bool CItemData::SetPWPolicy(const stringT &cs_pwp)
{
  // Basic sanity checks
  if (cs_pwp.empty()) {
    CItem::SetField(POLICY, cs_pwp.c_str());
    return true;
  }

  const StringX cs_pwpolicy(cs_pwp.c_str());
  PWPolicy pwp(cs_pwpolicy);
  PWPolicy emptyPol;
  // a non-empty string creates an empty policy iff it's ill-formed
  if (pwp == emptyPol)
    return false;

  CItem::SetField(POLICY, cs_pwpolicy);
  return true;
}

void CItemData::SetDCA(int16 iDCA, const bool bShift)
{
  unsigned char buf[sizeof(int16)];
  putInt(buf, iDCA);
  CItem::SetField(bShift ? SHIFTDCA : DCA, buf, sizeof(int16));
}

bool CItemData::SetDCA(const stringT &cs_DCA, const bool bShift)
{
  int16 iDCA(-1);

  if (cs_DCA.empty()) {
    SetDCA(iDCA, bShift);
    return true;
  }

  if (cs_DCA.find_first_not_of(_T("0123456789")) == stringT::npos) {
    istringstreamT is(cs_DCA);
    is >> iDCA;
    if (is.fail())
      return false;
    if (iDCA == -1 || (iDCA >= PWSprefs::minDCA && iDCA <= PWSprefs::maxDCA)) {
      SetDCA(iDCA, bShift);
      return true;
    }
  }
  return false;
}

void CItemData::SetProtected(bool bOnOff)
{
  if (bOnOff) {
    const unsigned char ucProtected = 1;
    CItem::SetField(PROTECTED, &ucProtected, sizeof(char));
  } else { // remove field
    m_fields.erase(PROTECTED);
  }
}

void CItemData::SetKBShortcut(int32 iKBShortcut)
{
  unsigned char buf[sizeof(int32)];
  putInt(buf, iKBShortcut);
  CItem::SetField(KBSHORTCUT, buf, sizeof(int32));
}

void CItemData::SetKBShortcut(const StringX &sx_KBShortcut)
{
  int32 iKBShortcut(0);
  WORD wVirtualKeyCode(0);
  WORD wPWSModifiers(0);
  size_t len = sx_KBShortcut.length();
  if (!sx_KBShortcut.empty()) {
    for (size_t i = 0; i < len; i++) {
      if (sx_KBShortcut.substr(i, 1) == _T(":")) {
        // 4 hex digits should follow the colon
        ASSERT(i + 5 == len);
        istringstreamT iss(sx_KBShortcut.substr(i + 1, 4).c_str());
        iss >> hex >> wVirtualKeyCode;
        break;
      }
      if (sx_KBShortcut.substr(i, 1) == _T("A"))
        wPWSModifiers |= PWS_HOTKEYF_ALT;
      if (sx_KBShortcut.substr(i, 1) == _T("C"))
        wPWSModifiers |= PWS_HOTKEYF_CONTROL;
      if (sx_KBShortcut.substr(i, 1) == _T("S"))
        wPWSModifiers |= PWS_HOTKEYF_SHIFT;
      if (sx_KBShortcut.substr(i, 1) == _T("E"))
        wPWSModifiers |= PWS_HOTKEYF_EXT;
      if (sx_KBShortcut.substr(i, 1) == _T("M"))
        wPWSModifiers |= PWS_HOTKEYF_META;
      if (sx_KBShortcut.substr(i, 1) == _T("W"))
        wPWSModifiers |= PWS_HOTKEYF_WIN;
      if (sx_KBShortcut.substr(i, 1) == _T("D"))
        wPWSModifiers |= PWS_HOTKEYF_CMD;
    }
  }

  if (wPWSModifiers != 0 && wVirtualKeyCode != 0) {
    iKBShortcut = (wPWSModifiers << 16) + wVirtualKeyCode;
  }

  SetKBShortcut(iKBShortcut);
}

void CItemData::SetFieldValue(FieldType ft, const StringX &value)
{
  switch (ft) {
    case GROUP:      /* 02 */
    case TITLE:      /* 03 */
    case USER:       /* 04 */
    case NOTES:      /* 05 */
    case PASSWORD:   /* 06 */
    case TWOFACTORKEY: /* 21 */
    case URL:        /* 0d */
    case AUTOTYPE:   /* 0e */
    case PWHIST:     /* 0f */
    case EMAIL:      /* 14 */
    case RUNCMD:     /* 12 */
    case SYMBOLS:    /* 16 */
    case POLICYNAME: /* 18 */
      CItem::SetField(ft, value);
      break;
    case TOTPCONFIG:
    case TOTPLENGTH:
    case TOTPTIMESTEP:
      SetFieldAsByte(ft, value.c_str());
      break;
    case TOTPSTARTTIME:
      SetTime(ft, value.c_str(), true);
      break;
    case CTIME:      /* 07 */
    case PMTIME:     /* 08 */
    case ATIME:      /* 09 */
    case XTIME:      /* 0a */
    case RMTIME:     /* 0c */
      SetTime(ft, value.c_str());
      break;
    case POLICY:     /* 10 */
      SetPWPolicy(value.c_str());
      break;
    case XTIME_INT:  /* 11 */
      SetXTimeInt(value.c_str());
      break;
    case DCA:        /* 13 */
      SetDCA(value.c_str());
      break;
    case PROTECTED:  /* 15 */
      {
        StringX sxProtected = _T("");
        LoadAString(sxProtected, IDSC_YES);
        SetProtected(value.compare(_T("1")) == 0 || value.compare(sxProtected) == 0);
      }
      break;
    case SHIFTDCA:   /* 17 */
      SetShiftDCA(value.c_str());
      break;
    case KBSHORTCUT: /* 19 */
      SetKBShortcut(value);
      break;
    case GROUPTITLE: /* 00 */
    case UUID:       /* 01 */
    case RESERVED:   /* 0b */
    default:
      ASSERT(0);     /* Not supported */
  }
}

bool CItemData::ValidatePWHistory()
{
  // Return true if valid
  if (!IsPasswordHistorySet())
    return true; // empty is a kind of valid

  const StringX pwh = GetPWHistory();
  if (pwh.length() < 5) { // not empty, but too short.
    SetPWHistory(_T(""));
    return false;
  }

  PWHistList pwhistlist(pwh, PWSUtil::TMC_EXPORT_IMPORT);
  if (pwhistlist.getErr() == 0)
    return true;

  if (pwhistlist.getErr() == static_cast<size_t>(-1)) { // unrecoverable error
    SetPWHistory(_T(""));
    return false;
  }

  size_t pwh_max = pwhistlist.getMax();
  size_t listnum = pwhistlist.size();

  if (pwh_max == 0 && listnum == 0) {
    SetPWHistory(_T(""));
    return false;
  }

  if (listnum > pwh_max)
    pwhistlist.setMax(listnum);

  // Rebuild PWHistory from the data we have
  StringX sxNewHistory = pwhistlist;
  if (pwh != sxNewHistory) {
    SetPWHistory(sxNewHistory);
    return false;
  }

  return true;
}

bool CItemData::Matches(const stringT &stValue, int iObject,
                        int iFunction) const
{
  ASSERT(iFunction != 0); // must be positive or negative!

  StringX sx_Object;
  auto ft = static_cast<FieldType>(iObject);
  switch(ft) {
    case GROUP:
    case TITLE:
    case USER:
    case URL:
    case NOTES:
    case PASSWORD:
    case TWOFACTORKEY:
    case RUNCMD:
    case EMAIL:
    case SYMBOLS:
    case POLICYNAME:
    case AUTOTYPE:
      sx_Object = GetField(ft);
      break;
    case GROUPTITLE:
      sx_Object = GetGroup() + TCHAR('.') + GetTitle();
      break;
    default:
      ASSERT(0);
  }

  const bool bValue = !sx_Object.empty();
  if (iFunction == PWSMatch::MR_PRESENT || iFunction == PWSMatch::MR_NOTPRESENT) {
    return PWSMatch::Match(bValue, iFunction);
  }

  return PWSMatch::Match(stValue.c_str(), sx_Object, iFunction);
}

bool CItemData::Matches(int num1, int num2, int iObject,
                        int iFunction) const
{
  //  Check integer values are selected
  int iValue;

  switch (iObject) {
    case XTIME_INT:
      GetXTimeInt(iValue);
      break;
    case ENTRYSIZE:
      iValue = static_cast<int>(GetSize());
      break;
    case PASSWORDLEN:
      iValue = static_cast<int>(GetPasswordLength());
      break;
    case KBSHORTCUT:
      GetKBShortcut(iValue);
      break;
    default:
      ASSERT(0);
      return false;
  }

  const bool bValue = (iValue != 0);
  if (iFunction == PWSMatch::MR_PRESENT || iFunction == PWSMatch::MR_NOTPRESENT)
    return PWSMatch::Match(bValue, iFunction);

  if (!bValue) // integer empty - always return false for other comparisons
    return false;
  else
    return PWSMatch::Match(num1, num2, iValue, iFunction);
}

bool CItemData::Matches(int16 dca, int iFunction, const bool bShift) const
{
  int16 iDCA;
  GetDCA(iDCA, bShift);
  if (iDCA < 0)
    iDCA = static_cast<int16>(PWSprefs::GetInstance()->GetPref(bShift ?
               PWSprefs::ShiftDoubleClickAction : PWSprefs::DoubleClickAction));

  switch (iFunction) {
    case PWSMatch::MR_IS:
      return iDCA == dca;
    case PWSMatch::MR_ISNOT:
      return iDCA != dca;
    case PWSMatch::MR_PRESENT:
      return IsDCASet(bShift);
    case PWSMatch::MR_NOTPRESENT:
      return ! IsDCASet(bShift);
    default:
      ASSERT(0);
  }
  return false;
}

bool CItemData::MatchesTime(time_t time1, time_t time2, int iObject,
                        int iFunction) const
{
  //   Check time values are selected
  time_t tValue;

  switch (iObject) {
    case CTIME:
    case PMTIME:
    case ATIME:
    case XTIME:
    case RMTIME:
      CItem::GetTime(iObject, tValue);
      break;
    default:
      ASSERT(0);
      return false;
  }

  const bool bValue = (tValue != time_t(0));
  if (iFunction == PWSMatch::MR_PRESENT || iFunction == PWSMatch::MR_NOTPRESENT) {
    return PWSMatch::Match(bValue, iFunction);
  }

  if (!bValue)  // date empty - always return false for other comparisons
    return false;
  else {
    time_t testtime = time_t(0);
    if (tValue) {
      struct tm st;
      errno_t err;
      err = localtime_s(&st, &tValue);
      ASSERT(err == 0);
      if (!err) {
        st.tm_hour = 0;
        st.tm_min = 0;
        st.tm_sec = 0;
        testtime = mktime(&st);
      }
    }
    return PWSMatch::Match(time1, time2, testtime, iFunction);
  }
}

bool CItemData::Matches(EntryType etype, int iFunction) const
{
  switch (iFunction) {
    case PWSMatch::MR_IS:
      return GetEntryType() == etype;
    case PWSMatch::MR_ISNOT:
      return GetEntryType() != etype;
    default:
      ASSERT(0);
  }
  return false;
}

bool CItemData::Matches(EntryStatus estatus, int iFunction) const
{
  switch (iFunction) {
    case PWSMatch::MR_IS:
      return GetStatus() == estatus;
    case PWSMatch::MR_ISNOT:
      return GetStatus() != estatus;
    default:
      ASSERT(0);
  }
  return false;
}

bool CItemData::IsExpired() const
{
  time_t now, XTime;
  time(&now);

  GetXTime(XTime);
  return (XTime && (XTime < now));
}

bool CItemData::WillExpire(const int numdays) const
{
  time_t now, exptime=time_t(-1), XTime;
  time(&now);

  GetXTime(XTime);
  // Check if there is an expiry date?
  if (!XTime)
    return false;

  // Ignore if already expired
  if (XTime <= now)
    return false;

  struct tm st;
  errno_t err;
  err = localtime_s(&st, &now);  // secure version
  ASSERT(err == 0);
  if (!err){
    st.tm_mday += numdays;
    exptime = mktime(&st);
  }
  if (exptime == time_t(-1))
    exptime = now;

  // Will it expire in numdays?
  return (XTime < exptime);
}

static bool pull(int32 &i, const unsigned char *data, size_t len)
{
  if (len == sizeof(int32)) {
    i = getInt32(data);
  } else {
    ASSERT(0);
    return false;
  }
  return true;
}

static bool pull(int16 &i16, const unsigned char *data, size_t len)
{
  if (len == sizeof(int16)) {
    i16 = getInt16(data);
  } else {
    ASSERT(0);
    return false;
  }
  return true;
}

static bool pull(unsigned char &value, const unsigned char *data, size_t len)
{
  if (len == sizeof(char)) {
    value = *data;
  } else {
    ASSERT(0);
    return false;
  }
  return true;
}

bool CItemData::DeSerializePlainText(const std::vector<char> &v)
{
  auto iter = v.begin();
  int emergencyExit = 255;

  while (iter != v.end()) {
    unsigned char type = *iter++;
    if (static_cast<uint32>(distance(v.end(), iter)) < sizeof(uint32)) {
      ASSERT(0); // type must ALWAYS be followed by length
      return false;
    }

    if (type == END) {
      if (IsFieldSet(UUID))
        m_entrytype = ET_NORMAL; // could be *base, but can't know that here...
      else if (IsFieldSet(ALIASUUID))
        m_entrytype = ET_ALIAS;
      else if (IsFieldSet(SHORTCUTUUID))
        m_entrytype = ET_SHORTCUT;
      return true; // happy end
    }

    uint32 len = *(reinterpret_cast<const uint32 *>(&(*iter)));
    ASSERT(len < v.size()); // sanity check
    iter += sizeof(uint32);

    if (--emergencyExit == 0) {
      ASSERT(0);
      return false;
    }

#ifdef PWS_BIG_ENDIAN
    unsigned char buf[len];
    memset(buf, 0, len*sizeof(buf[0]));
	  
    switch(type) {
      case CTIME:
      case PMTIME:
      case ATIME:
      case XTIME:
      case RMTIME:
      case DCA:
      case SHIFTDCA:
      case KBSHORTCUT:
      case XTIME_INT:

        memcpy(buf, &(*iter), len);
        byteswap(buf, buf + len - 1);

        if (!SetField(type, buf, len))
          return false;
        break;

      default:
        if (!SetField(type, reinterpret_cast<const unsigned char *>(&(*iter)), len))
          return false;
	break;
    }
#else
    if (!SetField(type, reinterpret_cast<const unsigned char *>(&(*iter)), len))
      return false;
#endif
    iter += len;
  }
  return false; // END tag not found!
}

bool CItemData::SetField(unsigned char ft_byte, const unsigned char* data, size_t len)
{
  auto ft = static_cast<FieldType>(ft_byte);
  return SetField(ft, data, len);
}

bool CItemData::SetField(CItem::FieldType ft, const unsigned char* data, size_t len)
{
  int32 i32;
  int16 i16;
  unsigned char uc;

  switch (ft) {
    case NAME:
      ASSERT(0); // not serialized, or in v3 format
      return false;
    case UUID:
    case BASEUUID:
    case ALIASUUID:
    case SHORTCUTUUID:
    case ATTREF:
      {
        uuid_array_t uuid_array;
        ASSERT(len == sizeof(uuid_array_t));
        if (data == nullptr || len < sizeof(uuid_array_t))
          return false;
        for (size_t i = 0; i < sizeof(uuid_array_t); i++)
          uuid_array[i] = data[i];
        SetUUID(uuid_array, ft);
        break;
      }
    case GROUP:
    case TITLE:
    case USER:
    case NOTES:
    case PASSWORD:
    case TWOFACTORKEY:
    case POLICY:
    case URL:
    case AUTOTYPE:
    case PWHIST:
    case RUNCMD:
    case EMAIL:
    case SYMBOLS:
    case POLICYNAME:
    case DATA_ATT_TITLE:
    case DATA_ATT_MEDIATYPE:
    case DATA_ATT_FILENAME:
    case PASSKEY_RP_ID:
      if (!SetTextField(ft, data, len)) return false;
      break;
    case TOTPCONFIG:
    case TOTPTIMESTEP:
    case TOTPLENGTH:
    case DATA_ATT_CONTENT:
    case PASSKEY_CRED_ID:
    case PASSKEY_USER_HANDLE:
    case PASSKEY_ALGO_ID:
    case PASSKEY_PRIVATE_KEY:
    case PASSKEY_SIGN_COUNT:
      CItem::SetField(ft, data, len);
      break;
    case CTIME:
    case PMTIME:
    case ATIME:
    case XTIME:
    case RMTIME:
    case TOTPSTARTTIME:
    case DATA_ATT_MTIME:
      if (!SetTimeField(ft, data, len)) return false;
      break;
    case XTIME_INT:
      if (!pull(i32, data, len)) return false;
      SetXTimeInt(i32);
      break;
    case DCA:
      if (!pull(i16, data, len)) return false;
      SetDCA(i16);
      break;
    case SHIFTDCA:
      if (!pull(i16, data, len)) return false;
      SetShiftDCA(i16);
      break;
    case PROTECTED:
      if (!pull(uc, data, len)) return false;
      SetProtected(uc != 0);
      break;
    case KBSHORTCUT:
      if (!pull(i32, data, sizeof(int32))) return false;
      SetKBShortcut(i32);
      break;
    case END:
      break;
    default:
      // unknowns!
      SetUnknownField(static_cast<unsigned char>(ft), len, data);
      break;
  }
  return true;
}

void CItemData::SetEntryType(EntryType et)
{
  /**
   * When changing between NORMAL (default) and shortcut/alias
   * we need to move the UUID to the correct field.
   * In other cases we leave the UUID untouched.
   */
  if (m_entrytype == ET_NORMAL || m_entrytype == ET_ALIASBASE || m_entrytype == ET_SHORTCUTBASE) {
    if (et == ET_ALIAS || et == ET_SHORTCUT) {
      const CUUID uuid = GetUUID(UUID);
      SetUUID(uuid, et == ET_ALIAS ? ALIASUUID : SHORTCUTUUID);
      m_fields.erase(UUID);
    }
  } else if (et == ET_NORMAL || m_entrytype == ET_ALIASBASE || m_entrytype == ET_SHORTCUTBASE) {
    if (m_entrytype == ET_ALIAS || m_entrytype == ET_SHORTCUT) {
      const CUUID uuid = GetUUID(m_entrytype == ET_ALIAS ? ALIASUUID : SHORTCUTUUID);
      SetUUID(uuid, UUID);
      m_fields.erase(m_entrytype == ET_ALIAS ? ALIASUUID : SHORTCUTUUID);
    }
  }
  m_entrytype = et;
}

void CItemData::SerializePlainText(vector<char> &v,
                                   const CItemData *pcibase)  const
{
  StringX tmp;
  uuid_array_t uuid_array;
  time_t t = 0;
  int32 i32 = 0;
  int16 i16 = 0;
  unsigned char uc = 0;

  v.clear();

  // We can be either regular, alias or shortcut, use the right uuid.
  const FieldType uuidfts[] = {UUID, ALIASUUID, SHORTCUTUUID};
  for (auto ft : uuidfts) {
    if (IsFieldSet(ft)) {
      GetUUID(uuid_array);
      v.push_back(static_cast<char>(ft));
      push_length(v, sizeof(uuid_array_t));
      v.insert(v.end(), uuid_array, (uuid_array + sizeof(uuid_array_t)));
      break;
    }
  }

  push(v, GROUP, GetGroup());
  push(v, TITLE, GetTitle());
  push(v, USER, GetUser());

  if (IsDependent()) {
    ASSERT(pcibase != nullptr);
    ASSERT(IsFieldSet(BASEUUID));
    ASSERT(GetBaseUUID() == pcibase->GetUUID());
    v.push_back(BASEUUID);
    GetUUID(uuid_array, BASEUUID);
    push_length(v, sizeof(uuid_array_t));
    v.insert(v.end(), uuid_array, (uuid_array + sizeof(uuid_array_t)));
  }

  tmp = ResolvePlaceholderEligibleField(this, pcibase, [this] { return GetPassword(); });
  push(v, PASSWORD, tmp);

  if (IsTotpActive()) {
    ASSERT(!GetTwoFactorKey().empty());
    push(v, TWOFACTORKEY, GetTwoFactorKey());
    if (!IsTotpConfigDefault())
      push(v, TOTPCONFIG, GetTotpConfig());
    if (!IsTotpStartTimeDefault())
      push(v, TOTPSTARTTIME, GetTotpStartTimeAsTimeT());
    if (!IsTotpTimeStepSecondsDefault())
      push(v, TOTPTIMESTEP, GetTotpTimeStepSeconds());
    if (!IsTotpLengthDefault())
      push(v, TOTPLENGTH, GetTotpLength());
  }

  push(v, NOTES, GetNotes());
  push(v, URL, GetURL());
  push(v, AUTOTYPE, GetAutoType());

  GetCTime(t);   push(v, CTIME, t);
  GetPMTime(t);  push(v, PMTIME, t);
  GetATime(t);   push(v, ATIME, t);
  GetXTime(t);   push(v, XTIME, t);
  GetRMTime(t);  push(v, RMTIME, t);

  GetXTimeInt(i32); push(v, XTIME_INT, i32);

  push(v, POLICY, GetPWPolicy());
  push(v, PWHIST, GetPWHistory());

  push(v, RUNCMD, GetRunCommand());
  GetDCA(i16); if (i16 != -1) push(v, DCA, i16);
  GetShiftDCA(i16); if (i16 != -1) push(v, SHIFTDCA, i16);
  push(v, EMAIL, GetEmail());
  GetProtected(uc); push(v, PROTECTED, uc);
  push(v, SYMBOLS, GetSymbols());
  push(v, POLICYNAME, GetPolicyName());
  GetKBShortcut(i32); push(v, KBSHORTCUT, i32);

  for (auto vi_IterURFE = m_URFL.begin();
       vi_IterURFE != m_URFL.end();
       vi_IterURFE++) {
    unsigned char type;
    size_t length = 0;
    unsigned char *pdata = nullptr;
    GetUnknownField(type, length, pdata, *vi_IterURFE);
    if (length != 0) {
      v.push_back(static_cast<char>(type));
      push_length(v, static_cast<uint32>(length));
      v.insert(v.end(), reinterpret_cast<char *>(pdata),
               reinterpret_cast<char *>(pdata) + length);
      trashMemory(pdata, length);
    }
    delete[] pdata;
  }

  int end = END; // just to keep the compiler happy...
  v.push_back(static_cast<char>(end));
  push_length(v, 0);
}

  // Convenience: Get the name associated with FieldType
stringT CItemData::FieldName(FieldType ft)
{
  // "User" fields only i.e. ft < CItem::LAST_USER_FIELD
  stringT retval(_T(""));
  switch (ft) {
  case GROUPTITLE:   LoadAString(retval, IDSC_FLDNMGROUPTITLE); break;
  case UUID:         LoadAString(retval, IDSC_FLDNMUUID); break;
  case GROUP:        LoadAString(retval, IDSC_FLDNMGROUP); break;
  case TITLE:        LoadAString(retval, IDSC_FLDNMTITLE); break;
  case USER:         LoadAString(retval, IDSC_FLDNMUSERNAME); break;
  case NOTES:        LoadAString(retval, IDSC_FLDNMNOTES); break;
  case PASSWORD:     LoadAString(retval, IDSC_FLDNMPASSWORD); break;
  case TWOFACTORKEY: LoadAString(retval, IDSC_FLDNMTWOFACTORKEY); break;
  case TOTPCONFIG:   LoadAString(retval, IDSC_FLDNMTOTPCONFIG); break;
  case TOTPSTARTTIME: LoadAString(retval, IDSC_FLDNMTOTPSTARTTIME); break;
  case TOTPTIMESTEP: LoadAString(retval, IDSC_FLDNMTOTPTIMESTEP); break;
  case TOTPLENGTH:   LoadAString(retval, IDSC_FLDNMTOTPLENGTH); break;
  case CTIME:        LoadAString(retval, IDSC_FLDNMCTIME); break;
  case PMTIME:       LoadAString(retval, IDSC_FLDNMPMTIME); break;
  case ATIME:        LoadAString(retval, IDSC_FLDNMATIME); break;
  case XTIME:        LoadAString(retval, IDSC_FLDNMXTIME); break;
  case RMTIME:       LoadAString(retval, IDSC_FLDNMRMTIME); break;
  case URL:          LoadAString(retval, IDSC_FLDNMURL); break;
  case AUTOTYPE:     LoadAString(retval, IDSC_FLDNMAUTOTYPE); break;
  case PWHIST:       LoadAString(retval, IDSC_FLDNMPWHISTORY); break;
  case POLICY:       LoadAString(retval, IDSC_FLDNMPWPOLICY); break;
  case XTIME_INT:    LoadAString(retval, IDSC_FLDNMXTIMEINT); break;
  case RUNCMD:       LoadAString(retval, IDSC_FLDNMRUNCOMMAND); break;
  case DCA:          LoadAString(retval, IDSC_FLDNMDCA); break;
  case SHIFTDCA:     LoadAString(retval, IDSC_FLDNMSHIFTDCA); break;
  case EMAIL:        LoadAString(retval, IDSC_FLDNMEMAIL); break;
  case PROTECTED:    LoadAString(retval, IDSC_FLDNMPROTECTED); break;
  case SYMBOLS:      LoadAString(retval, IDSC_FLDNMSYMBOLS); break;
  case POLICYNAME:   LoadAString(retval, IDSC_FLDNMPWPOLICYNAME); break;
  case KBSHORTCUT:   LoadAString(retval, IDSC_FLDNMKBSHORTCUT); break;
  case ATTREF:       LoadAString(retval, IDSC_FLDNMATTREF); break;
  case CCNUM:        LoadAString(retval, IDSC_FLDNMCCNUM); break;
  case CCEXP:        LoadAString(retval, IDSC_FLDNMCCEXP); break;
  case CCVV:         LoadAString(retval, IDSC_FLDNMCCVV); break;
  case CCPIN:        LoadAString(retval, IDSC_FLDNMCCPIN); break;
  case DATA_ATT_TITLE:     LoadAString(retval, IDSC_FLDNMDATAATTTITLE); break;
  case DATA_ATT_MEDIATYPE: LoadAString(retval, IDSC_FLDNMDATAATTMEDIATYPE); break;
  case DATA_ATT_FILENAME:  LoadAString(retval, IDSC_FLDNMDATAATTFILENAME); break;
  case DATA_ATT_MTIME:     LoadAString(retval, IDSC_FLDNMDATAATTMTIME); break;
  case DATA_ATT_CONTENT:   LoadAString(retval, IDSC_FLDNMDATAATTCONTENT); break;
  case PASSKEY_CRED_ID:     LoadAString(retval, IDSC_FLDNMPASSKEYCREDID); break;
  case PASSKEY_RP_ID:       LoadAString(retval, IDSC_FLDNMPASSKEYRPID); break;
  case PASSKEY_USER_HANDLE: LoadAString(retval, IDSC_FLDNMPASSKEYUSERHANDLE); break;
  case PASSKEY_ALGO_ID:     LoadAString(retval, IDSC_FLDNMPASSKEYALGOID); break;
  case PASSKEY_PRIVATE_KEY: LoadAString(retval, IDSC_FLDNMPASSKEYPRIVATEKEY); break;
  case PASSKEY_SIGN_COUNT:  LoadAString(retval, IDSC_FLDNMPASSKEYSIGNCOUNT); break;

  default:
    ASSERT(0);
  };
  return retval;
}
  // Convenience: Get the untranslated (English) name of a FieldType
stringT CItemData::EngFieldName(FieldType ft)
{
  switch (ft) {
  case GROUPTITLE:    return _T("Group/Title");
  case UUID:          return _T("UUID");
  case GROUP:         return _T("Group");
  case TITLE:         return _T("Title");
  case USER:          return _T("Username");
  case NOTES:         return _T("Notes");
  case PASSWORD:      return _T("Password");
  case TWOFACTORKEY:  return _T("Two Factor Key");
  case TOTPCONFIG:    return _T("TOTP Config");
  case TOTPSTARTTIME: return _T("TOTP Start Time");
  case TOTPTIMESTEP:  return _T("TOTP Time Step");
  case TOTPLENGTH:    return _T("TOTP Length");
  case CTIME:         return _T("Created Time");
  case PMTIME:        return _T("Password Modified Time");
  case ATIME:         return _T("Last Access Time");
  case XTIME:         return _T("Password Expiry Date");
  case RMTIME:        return _T("Record Modified Time");
  case URL:           return _T("URL");
  case AUTOTYPE:      return _T("AutoType");
  case PWHIST:        return _T("History");
  case POLICY:        return _T("Password Policy");
  case XTIME_INT:     return _T("Password Expiry Interval");
  case RUNCMD:        return _T("Run Command");
  case DCA:           return _T("DCA");
  case SHIFTDCA:      return _T("Shift+DCA");
  case EMAIL:         return _T("e-mail");
  case PROTECTED:     return _T("Protected");
  case SYMBOLS:       return _T("Symbols");
  case POLICYNAME:    return _T("Password Policy Name");
  case KBSHORTCUT:    return _T("Keyboard Shortcut");
  case ATTREF:        return _T("Attachment Reference");
  case BASEUUID:      return _T("Base UUID");
  case ALIASUUID:     return _T("Alias UUID");
  case SHORTCUTUUID:  return _T("Shortcut UUID");
  case UNKNOWNFIELDS: return _T("Unknown");
  default:
    ASSERT(0);
    return _T("");
  };
}

std::string CItemData::GetXmlFieldName(FieldType ft)
{
  return toutf8(GetXmlFieldNameW(ft));
}

std::wstring CItemData::GetXmlFieldNameW(FieldType ft)
{
  stringT s = EngFieldName(ft);
  ASSERT(!s.empty());
  if (!s.empty()) {
    s.erase(std::remove_if(s.begin(), s.end(), ::isspace), s.end());
    for (auto& c : s)
      c = static_cast<char>(tolower(c));
  }
  return s;
}

stringT CItemData::GetUserInterfaceFieldName(FieldType ft)
{
  stringT retval(_T(""));
  switch (ft) {
  case TWOFACTORKEY: LoadAString(retval, IDSC_FLDNMTWOFACTORKEY_UI); break;
  default:
    // This default returns the FieldName as a placeholder. If you intend
    // to use this method, put a valid 'case' above for the desired field
    // to indicate the intention to define a UI field name regardless of
    // whether or not it differs from the default field name.
    ASSERT(FALSE);
    retval = FieldName(ft);
  }
  return retval;
}


StringX CItemData::GetTotpAuthCode(time_t* pBasisTimeNow, double* pRatioExpired) const
{
  StringX retval;
  if (PWSTotp::GetNextTotpAuthCodeString(*this, retval, pBasisTimeNow, pRatioExpired) != PWSTotp::Success)
  {
    retval.clear();
  }
  return retval;
}

size_t CItemData::GetAttContentLength() const {
  auto fiter = m_fields.find(DATA_ATT_CONTENT);

  if (fiter != m_fields.end())
    return fiter->second.GetLength();
  else
    return 0;
}

std::vector<unsigned char> CItemData::GetAttContent() const {
  std::vector<unsigned char> v;
  GetField(DATA_ATT_CONTENT, v);
  return v;
}

void CItemData::ClearAttachment() {
  ClearField(DATA_ATT_TITLE);
  ClearField(DATA_ATT_MEDIATYPE);
  ClearField(DATA_ATT_FILENAME);
  ClearField(DATA_ATT_MTIME);
  ClearField(DATA_ATT_CONTENT);
}

int32 CItemData::GetPasskeyAlgorithmID() const {
    std::vector<unsigned char> v;
    GetField(PASSKEY_ALGO_ID, v);
    ASSERT(v.size() == 4 || v.size() == 0);
    return v.size() == 4 ? getInt32(v.data()) : 0;
}

uint32 CItemData::GetPasskeySignCount() const {
    std::vector<unsigned char> v;
    GetField(PASSKEY_SIGN_COUNT, v);
    ASSERT(v.size() == 4 || v.size() == 0);
    return v.size() == 4 ? getInt32(v.data()) : 0;
}

VectorX<unsigned char> CItemData::GetPasskeyCredentialID() const {
    VectorX<unsigned char> v;
    GetField(PASSKEY_CRED_ID, v);
    return v;
}

VectorX<unsigned char> CItemData::GetPasskeyUserHandle() const {
    VectorX<unsigned char> v;
    GetField(PASSKEY_USER_HANDLE, v);
    return v;
}

VectorX<unsigned char> CItemData::GetPasskeyPrivateKey() const {
    VectorX<unsigned char> v;
    GetField(PASSKEY_PRIVATE_KEY, v);
    return v;
}

void CItemData::SetPasskeyAlgorithmID(const int32 algo_id) {
    unsigned char buf[4];
    putInt32(buf, algo_id);
    CItem::SetField(PASSKEY_ALGO_ID, buf, 4);
}

void CItemData::SetPasskeySignCount(const uint32 sign_count) {
    unsigned char buf[4];
    putInt32(buf, sign_count);
    CItem::SetField(PASSKEY_SIGN_COUNT, buf, 4);
}

bool CItemData::HasIncompletePasskey() const {
    constexpr static std::array<int, 6> fields = {
        PASSKEY_CRED_ID,
        PASSKEY_RP_ID,
        PASSKEY_USER_HANDLE,
        PASSKEY_ALGO_ID,
        PASSKEY_PRIVATE_KEY,
        PASSKEY_SIGN_COUNT
    };
    auto numSet = 0;
    for (int ft : fields)
        numSet += IsFieldSet(ft) ? 1 : 0;
    return !(numSet == 0 || numSet == fields.size());
}

void CItemData::ClearPasskey() {
    ClearField(PASSKEY_CRED_ID);
    ClearField(PASSKEY_RP_ID);
    ClearField(PASSKEY_USER_HANDLE);
    ClearField(PASSKEY_ALGO_ID);
    ClearField(PASSKEY_PRIVATE_KEY);
    ClearField(PASSKEY_SIGN_COUNT);
}
