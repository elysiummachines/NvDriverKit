/* Security primitives.
 *
 * This process runs elevated and launches other executables, so the three
 * things that actually matter are implemented here:
 *   - prove an image is the one we expect (Authenticode + signer pin + hash)
 *   - refuse to execute anything a non-admin could have replaced (DACL walk)
 *   - keep our own state in a directory only SYSTEM and Administrators can write
 */
#include "common.h"
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <aclapi.h>
#include <sddl.h>
#include <stdio.h>

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

/* ------------------------------------------------------------------ hashing */

BOOL sec_sha256_file(const wchar_t *path, wchar_t *out_hex, size_t out_cch)
{
    BCRYPT_ALG_HANDLE  alg  = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    HANDLE   fh = INVALID_HANDLE_VALUE;
    UCHAR   *obj = NULL;
    UCHAR    digest[32];
    DWORD    obj_len = 0, cb = 0, got = 0;
    BYTE    *buf = NULL;
    BOOL     ok = FALSE;
    size_t   i;

    if (out_cch < 65) return FALSE;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != STATUS_SUCCESS)
        goto done;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&obj_len,
                          sizeof(obj_len), &cb, 0) != STATUS_SUCCESS)
        goto done;
    obj = (UCHAR *)HeapAlloc(GetProcessHeap(), 0, obj_len);
    if (!obj) goto done;
    if (BCryptCreateHash(alg, &hash, obj, obj_len, NULL, 0, 0) != STATUS_SUCCESS)
        goto done;

    fh = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                     FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) goto done;

    buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, 65536);
    if (!buf) goto done;

    for (;;) {
        if (!ReadFile(fh, buf, 65536, &got, NULL)) goto done;
        if (got == 0) break;
        if (BCryptHashData(hash, buf, got, 0) != STATUS_SUCCESS) goto done;
    }
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) != STATUS_SUCCESS)
        goto done;

    for (i = 0; i < 32; i++)
        _snwprintf(out_hex + i * 2, 3, L"%02x", digest[i]);
    out_hex[64] = L'\0';
    ok = TRUE;

done:
    if (buf)  HeapFree(GetProcessHeap(), 0, buf);
    if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
    if (hash) BCryptDestroyHash(hash);
    if (obj)  HeapFree(GetProcessHeap(), 0, obj);
    if (alg)  BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

/* ------------------------------------------------------------ authenticode */

BOOL sec_verify_authenticode(const wchar_t *path)
{
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_FILE_INFO fi;
    WINTRUST_DATA wd;
    LONG status;

    ZeroMemory(&fi, sizeof(fi));
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = path;

    ZeroMemory(&wd, sizeof(wd));
    wd.cbStruct            = sizeof(wd);
    wd.dwUIChoice          = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    wd.dwUnionChoice       = WTD_CHOICE_FILE;
    wd.pFile               = &fi;
    wd.dwStateAction       = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags         = WTD_SAFER_FLAG | WTD_CACHE_ONLY_URL_RETRIEVAL;

    status = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);

    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);

    /* Offline revocation checking is allowed to be inconclusive; an actually
     * revoked or untrusted chain still fails. */
    if (status == ERROR_SUCCESS) return TRUE;
    if (status == (LONG)CERT_E_REVOCATION_FAILURE) {
        log_write(LG_WARN, L"Revocation could not be checked offline for %s", path);
        return TRUE;
    }
    log_write(LG_FAIL, L"Authenticode verification failed (0x%08lX): %s",
              (unsigned long)status, path);
    return FALSE;
}

/* Pull the signer certificate out of a signed PE. */
static PCCERT_CONTEXT signer_cert(const wchar_t *path, HCERTSTORE *out_store,
                                  HCRYPTMSG *out_msg)
{
    HCERTSTORE   store = NULL;
    HCRYPTMSG    msg   = NULL;
    DWORD        enc = 0, ctype = 0, fmt = 0, si_len = 0;
    CMSG_SIGNER_INFO *si = NULL;
    CERT_INFO    ci;
    PCCERT_CONTEXT cert = NULL;

    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path,
                          CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                          CERT_QUERY_FORMAT_FLAG_BINARY, 0,
                          &enc, &ctype, &fmt, &store, &msg, NULL))
        return NULL;

    if (!CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, NULL, &si_len))
        goto fail;
    si = (CMSG_SIGNER_INFO *)HeapAlloc(GetProcessHeap(), 0, si_len);
    if (!si) goto fail;
    if (!CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, si, &si_len))
        goto fail;

    ZeroMemory(&ci, sizeof(ci));
    ci.Issuer       = si->Issuer;
    ci.SerialNumber = si->SerialNumber;

    cert = CertFindCertificateInStore(store, enc, 0, CERT_FIND_SUBJECT_CERT,
                                      &ci, NULL);
    HeapFree(GetProcessHeap(), 0, si);
    if (!cert) goto fail;

    *out_store = store;
    *out_msg   = msg;
    return cert;

fail:
    if (si)    HeapFree(GetProcessHeap(), 0, si);
    if (msg)   CryptMsgClose(msg);
    if (store) CertCloseStore(store, 0);
    return NULL;
}

BOOL sec_signer_thumbprint(const wchar_t *path, wchar_t *out_hex, size_t out_cch)
{
    HCERTSTORE store = NULL;
    HCRYPTMSG  msg   = NULL;
    PCCERT_CONTEXT cert;
    BYTE  tp[20];
    DWORD tp_len = sizeof(tp);
    size_t i;

    if (out_cch < 41) return FALSE;

    cert = signer_cert(path, &store, &msg);
    if (!cert) return FALSE;

    if (!CertGetCertificateContextProperty(cert, CERT_SHA1_HASH_PROP_ID, tp, &tp_len)) {
        CertFreeCertificateContext(cert);
        CryptMsgClose(msg);
        CertCloseStore(store, 0);
        return FALSE;
    }
    for (i = 0; i < 20; i++)
        _snwprintf(out_hex + i * 2, 3, L"%02X", tp[i]);
    out_hex[40] = L'\0';

    CertFreeCertificateContext(cert);
    CryptMsgClose(msg);
    CertCloseStore(store, 0);
    return TRUE;
}

BOOL sec_signer_cn(const wchar_t *path, wchar_t *out, size_t out_cch)
{
    HCERTSTORE store = NULL;
    HCRYPTMSG  msg   = NULL;
    PCCERT_CONTEXT cert;
    DWORD n;

    cert = signer_cert(path, &store, &msg);
    if (!cert) return FALSE;

    n = CertGetNameStringW(cert, CERT_NAME_ATTR_TYPE, 0, (void *)szOID_COMMON_NAME,
                           out, (DWORD)out_cch);
    CertFreeCertificateContext(cert);
    CryptMsgClose(msg);
    CertCloseStore(store, 0);
    return n > 1;
}

/* ------------------------------------------------------------- DACL review */

/* SIDs allowed to hold write access on anything we execute. */
static BOOL sid_is_trusted(PSID sid)
{
    static const WELL_KNOWN_SID_TYPE ok[] = {
        WinLocalSystemSid,
        WinBuiltinAdministratorsSid,
        WinCreatorOwnerSid        /* owner ACE on an admin-owned object */
    };
    size_t i;
    PSID ti = NULL;

    for (i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        BYTE  buf[SECURITY_MAX_SID_SIZE];
        DWORD cb = sizeof(buf);
        if (CreateWellKnownSid(ok[i], NULL, buf, &cb) && EqualSid(sid, (PSID)buf))
            return TRUE;
    }
    /* NT SERVICE\TrustedInstaller */
    if (ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-"
                               L"1853292631-2271478464", &ti)) {
        BOOL eq = EqualSid(sid, ti);
        LocalFree(ti);
        if (eq) return TRUE;
    }
    return FALSE;
}

#define DANGEROUS_WRITE (FILE_WRITE_DATA | FILE_APPEND_DATA | WRITE_DAC | \
                         WRITE_OWNER | DELETE | FILE_WRITE_ATTRIBUTES)

static BOOL object_is_nonadmin_writable(const wchar_t *path, BOOL *out)
{
    PACL  dacl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    DWORD rc;
    WORD  i;

    *out = FALSE;

    rc = GetNamedSecurityInfoW(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                               NULL, NULL, &dacl, NULL, &sd);
    if (rc != ERROR_SUCCESS)
        return FALSE;

    if (!dacl) {                 /* NULL DACL == everyone has everything */
        *out = TRUE;
        LocalFree(sd);
        return TRUE;
    }

    for (i = 0; i < dacl->AceCount; i++) {
        ACCESS_ALLOWED_ACE *ace = NULL;
        if (!GetAce(dacl, i, (void **)&ace))
            continue;
        if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE)
            continue;
        if ((ace->Mask & DANGEROUS_WRITE) == 0 &&
            (ace->Mask & GENERIC_WRITE)   == 0 &&
            (ace->Mask & GENERIC_ALL)     == 0)
            continue;
        if (!sid_is_trusted((PSID)&ace->SidStart)) {
            *out = TRUE;
            break;
        }
    }

    LocalFree(sd);
    return TRUE;
}

BOOL sec_path_is_nonadmin_writable(const wchar_t *path, BOOL *out_writable)
{
    wchar_t parent[NVDK_MAX_PATH];
    wchar_t *slash;
    BOOL w = FALSE;

    *out_writable = FALSE;

    if (!object_is_nonadmin_writable(path, &w))
        return FALSE;
    if (w) { *out_writable = TRUE; return TRUE; }

    /* A writable parent directory means the file can simply be replaced. */
    wcsncpy(parent, path, NVDK_MAX_PATH - 1);
    parent[NVDK_MAX_PATH - 1] = L'\0';
    slash = wcsrchr(parent, L'\\');
    if (!slash) return TRUE;
    *slash = L'\0';
    if (!object_is_nonadmin_writable(parent, &w))
        return FALSE;

    *out_writable = w;
    return TRUE;
}

BOOL sec_harden_directory(const wchar_t *dir)
{
    /* Protected (P) DACL, inheritable full control for SYSTEM, Administrators
     * and Builtin Users read+execute only. */
    static const wchar_t *sddl =
        L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)";
    PSECURITY_DESCRIPTOR sd = NULL;
    PACL  dacl = NULL;
    BOOL  present = FALSE, defaulted = FALSE;
    DWORD rc;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1,
                                                              &sd, NULL))
        return FALSE;
    if (!GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted)) {
        LocalFree(sd);
        return FALSE;
    }
    rc = SetNamedSecurityInfoW((wchar_t *)dir, SE_FILE_OBJECT,
                               DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                               NULL, NULL, dacl, NULL);
    LocalFree(sd);
    return rc == ERROR_SUCCESS;
}

BOOL sec_is_elevated(void)
{
    HANDLE tok = NULL;
    TOKEN_ELEVATION el;
    DWORD cb = sizeof(el);
    BOOL ok = FALSE;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        return FALSE;
    if (GetTokenInformation(tok, TokenElevation, &el, cb, &cb))
        ok = el.TokenIsElevated != 0;
    CloseHandle(tok);
    return ok;
}
