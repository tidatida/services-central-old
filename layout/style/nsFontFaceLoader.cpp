/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
// vim:cindent:ts=2:et:sw=2:
/* ***** BEGIN LICENSE BLOCK *****
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
 * The Original Code is Mozilla Foundation code.
 *
 * The Initial Developer of the Original Code is
 * Mozilla Foundation.
 * Portions created by the Initial Developer are Copyright (C) 2008
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   John Daggett <jdaggett@mozilla.com>
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

/* code for loading in @font-face defined font data */

#ifdef MOZ_LOGGING
#define FORCE_PR_LOG /* Allow logging in the release build */
#endif /* MOZ_LOGGING */
#include "prlog.h"

#include "nsFontFaceLoader.h"

#include "nsError.h"
#include "nsIFile.h"
#include "nsILocalFile.h"
#include "nsIStreamListener.h"
#include "nsNetUtil.h"
#include "nsIChannelEventSink.h"
#include "nsIInterfaceRequestor.h"
#include "nsContentUtils.h"
#include "mozilla/Preferences.h"

#include "nsPresContext.h"
#include "nsIPresShell.h"
#include "nsIDocument.h"
#include "nsIFrame.h"
#include "nsIPrincipal.h"
#include "nsIScriptSecurityManager.h"

#include "nsDirectoryServiceUtils.h"
#include "nsDirectoryServiceDefs.h"
#include "nsIContentPolicy.h"
#include "nsContentPolicyUtils.h"
#include "nsContentErrors.h"
#include "nsCrossSiteListenerProxy.h"
#include "nsIContentSecurityPolicy.h"
#include "nsIChannelPolicy.h"
#include "nsChannelPolicy.h"

#include "nsIConsoleService.h"

#include "nsStyleSet.h"

using namespace mozilla;

#ifdef PR_LOGGING
static PRLogModuleInfo *gFontDownloaderLog = PR_NewLogModule("fontdownloader");
#endif /* PR_LOGGING */

#define LOG(args) PR_LOG(gFontDownloaderLog, PR_LOG_DEBUG, args)
#define LOG_ENABLED() PR_LOG_TEST(gFontDownloaderLog, PR_LOG_DEBUG)


nsFontFaceLoader::nsFontFaceLoader(gfxProxyFontEntry *aProxy, nsIURI *aFontURI,
                                   nsUserFontSet *aFontSet, nsIChannel *aChannel)
  : mFontEntry(aProxy), mFontURI(aFontURI), mFontSet(aFontSet),
    mChannel(aChannel)
{
}

nsFontFaceLoader::~nsFontFaceLoader()
{
  if (mLoadTimer) {
    mLoadTimer->Cancel();
    mLoadTimer = nsnull;
  }
  if (mFontSet) {
    mFontSet->RemoveLoader(this);
  }
}

void
nsFontFaceLoader::StartedLoading(nsIStreamLoader *aStreamLoader)
{
  PRInt32 loadTimeout =
    Preferences::GetInt("gfx.downloadable_fonts.fallback_delay", 3000);
  if (loadTimeout > 0) {
    mLoadTimer = do_CreateInstance("@mozilla.org/timer;1");
    if (mLoadTimer) {
      mLoadTimer->InitWithFuncCallback(LoadTimerCallback,
                                       static_cast<void*>(this),
                                       loadTimeout,
                                       nsITimer::TYPE_ONE_SHOT);
    }
  } else {
    mFontEntry->mLoadingState = gfxProxyFontEntry::LOADING_SLOWLY;
  }
  mStreamLoader = aStreamLoader;
}

void
nsFontFaceLoader::LoadTimerCallback(nsITimer *aTimer, void *aClosure)
{
  nsFontFaceLoader *loader = static_cast<nsFontFaceLoader*>(aClosure);

  gfxProxyFontEntry *pe = loader->mFontEntry.get();
  bool updateUserFontSet = true;

  // If the entry is loading, check whether it's >75% done; if so,
  // we allow another timeout period before showing a fallback font.
  if (pe->mLoadingState == gfxProxyFontEntry::LOADING_STARTED) {
    PRInt32 contentLength;
    loader->mChannel->GetContentLength(&contentLength);
    PRUint32 numBytesRead;
    loader->mStreamLoader->GetNumBytesRead(&numBytesRead);

    if (contentLength > 0 &&
        numBytesRead > 3 * (PRUint32(contentLength) >> 2))
    {
      // More than 3/4 the data has been downloaded, so allow 50% extra
      // time and hope the remainder will arrive before the additional
      // time expires.
      pe->mLoadingState = gfxProxyFontEntry::LOADING_ALMOST_DONE;
      PRUint32 delay;
      loader->mLoadTimer->GetDelay(&delay);
      loader->mLoadTimer->InitWithFuncCallback(LoadTimerCallback,
                                               static_cast<void*>(loader),
                                               delay >> 1,
                                               nsITimer::TYPE_ONE_SHOT);
      updateUserFontSet = false;
      LOG(("fontdownloader (%p) 75%% done, resetting timer\n", loader));
    }
  }

  // If the font is not 75% loaded, or if we've already timed out once
  // before, we mark this entry as "loading slowly", so the fallback
  // font will be used in the meantime, and tell the context to refresh.
  if (updateUserFontSet) {
    pe->mLoadingState = gfxProxyFontEntry::LOADING_SLOWLY;
    nsPresContext *ctx = loader->mFontSet->GetPresContext();
    NS_ASSERTION(ctx, "fontSet doesn't have a presContext?");
    gfxUserFontSet *fontSet;
    if (ctx && (fontSet = ctx->GetUserFontSet()) != nsnull) {
      fontSet->IncrementGeneration();
      ctx->UserFontSetUpdated();
      LOG(("fontdownloader (%p) timeout reflow\n", loader));
    }
  }
}

NS_IMPL_ISUPPORTS1(nsFontFaceLoader, nsIStreamLoaderObserver)

NS_IMETHODIMP
nsFontFaceLoader::OnStreamComplete(nsIStreamLoader* aLoader,
                                   nsISupports* aContext,
                                   nsresult aStatus,
                                   PRUint32 aStringLen,
                                   const PRUint8* aString)
{
  if (!mFontSet) {
    // We've been canceled
    return aStatus;
  }

  mFontSet->RemoveLoader(this);

#ifdef PR_LOGGING
  if (LOG_ENABLED()) {
    nsCAutoString fontURI;
    mFontURI->GetSpec(fontURI);
    if (NS_SUCCEEDED(aStatus)) {
      LOG(("fontdownloader (%p) download completed - font uri: (%s)\n", 
           this, fontURI.get()));
    } else {
      LOG(("fontdownloader (%p) download failed - font uri: (%s) error: %8.8x\n", 
           this, fontURI.get(), aStatus));
    }
  }
#endif

  nsPresContext *ctx = mFontSet->GetPresContext();
  NS_ASSERTION(ctx && !ctx->PresShell()->IsDestroying(),
               "We should have been canceled already");

  // whether an error occurred or not, notify the user font set of the completion
  gfxUserFontSet *userFontSet = ctx->GetUserFontSet();
  if (!userFontSet) {
    return aStatus;
  }

  if (NS_SUCCEEDED(aStatus)) {
    // for HTTP requests, check whether the request _actually_ succeeded;
    // the "request status" in aStatus does not necessarily indicate this,
    // because HTTP responses such as 404 (Not Found) will still result in
    // a success code and potentially an HTML error page from the server
    // as the resulting data. We don't want to use that as a font.
    nsCOMPtr<nsIHttpChannel> httpChannel = do_QueryInterface(mChannel);
    if (httpChannel) {
      PRBool succeeded;
      nsresult rv = httpChannel->GetRequestSucceeded(&succeeded);
      if (NS_SUCCEEDED(rv) && !succeeded) {
        aStatus = NS_ERROR_NOT_AVAILABLE;
      }
    }
  }

  // The userFontSet is responsible for freeing the downloaded data
  // (aString) when finished with it; the pointer is no longer valid
  // after OnLoadComplete returns.
  // This is called even in the case of a failed download (HTTP 404, etc),
  // as there may still be data to be freed (e.g. an error page),
  // and we need the fontSet to initiate loading the next source.
  PRBool fontUpdate = userFontSet->OnLoadComplete(mFontEntry,
                                                  aString, aStringLen,
                                                  aStatus);

  // when new font loaded, need to reflow
  if (fontUpdate) {
    // Update layout for the presence of the new font.  Since this is
    // asynchronous, reflows will coalesce.
    ctx->UserFontSetUpdated();
    LOG(("fontdownloader (%p) reflow\n", this));
  }

  return NS_SUCCESS_ADOPTED_DATA;
}

void
nsFontFaceLoader::Cancel()
{
  mFontEntry->mLoadingState = gfxProxyFontEntry::NOT_LOADING;
  mFontSet = nsnull;
  if (mLoadTimer) {
    mLoadTimer->Cancel();
    mLoadTimer = nsnull;
  }
  mChannel->Cancel(NS_BINDING_ABORTED);
}

nsresult
nsFontFaceLoader::CheckLoadAllowed(nsIPrincipal* aSourcePrincipal,
                                   nsIURI* aTargetURI,
                                   nsISupports* aContext)
{
  nsresult rv;
  
  if (!aSourcePrincipal)
    return NS_OK;

  // check with the security manager
  nsIScriptSecurityManager *secMan = nsContentUtils::GetSecurityManager();
  rv = secMan->CheckLoadURIWithPrincipal(aSourcePrincipal, aTargetURI,
                                        nsIScriptSecurityManager::STANDARD);
  if (NS_FAILED(rv)) {
    return rv;
  }

  // check content policy
  PRInt16 shouldLoad = nsIContentPolicy::ACCEPT;
  rv = NS_CheckContentLoadPolicy(nsIContentPolicy::TYPE_FONT,
                                 aTargetURI,
                                 aSourcePrincipal,
                                 aContext,
                                 EmptyCString(), // mime type
                                 nsnull,
                                 &shouldLoad,
                                 nsContentUtils::GetContentPolicy(),
                                 nsContentUtils::GetSecurityManager());

  if (NS_FAILED(rv) || NS_CP_REJECTED(shouldLoad)) {
    return NS_ERROR_CONTENT_BLOCKED;
  }

  return NS_OK;
}
  
nsUserFontSet::nsUserFontSet(nsPresContext *aContext)
  : mPresContext(aContext)
{
  NS_ASSERTION(mPresContext, "null context passed to nsUserFontSet");
  mLoaders.Init();
}

nsUserFontSet::~nsUserFontSet()
{
  NS_ASSERTION(mLoaders.Count() == 0, "mLoaders should have been emptied");
}

static PLDHashOperator DestroyIterator(nsPtrHashKey<nsFontFaceLoader>* aKey,
                                       void* aUserArg)
{
  aKey->GetKey()->Cancel();
  return PL_DHASH_REMOVE;
}

void
nsUserFontSet::Destroy()
{
  mPresContext = nsnull;
  mLoaders.EnumerateEntries(DestroyIterator, nsnull);
}

void
nsUserFontSet::RemoveLoader(nsFontFaceLoader *aLoader)
{
  mLoaders.RemoveEntry(aLoader);
}

nsresult 
nsUserFontSet::StartLoad(gfxProxyFontEntry *aProxy,
                         const gfxFontFaceSrc *aFontFaceSrc)
{
  nsresult rv;
  
  // check same-site origin
  nsIPresShell *ps = mPresContext->PresShell();
  if (!ps)
    return NS_ERROR_FAILURE;
    
  NS_ASSERTION(aFontFaceSrc && !aFontFaceSrc->mIsLocal, 
               "bad font face url passed to fontloader");
  NS_ASSERTION(aFontFaceSrc->mURI, "null font uri");
  if (!aFontFaceSrc->mURI)
    return NS_ERROR_FAILURE;

  // use document principal, original principal if flag set
  // this enables user stylesheets to load font files via
  // @font-face rules
  nsCOMPtr<nsIPrincipal> principal = ps->GetDocument()->NodePrincipal();

  NS_ASSERTION(aFontFaceSrc->mOriginPrincipal, 
               "null origin principal in @font-face rule");
  if (aFontFaceSrc->mUseOriginPrincipal) {
    principal = do_QueryInterface(aFontFaceSrc->mOriginPrincipal);
  }
  
  rv = nsFontFaceLoader::CheckLoadAllowed(principal, aFontFaceSrc->mURI, 
                                          ps->GetDocument());
  if (NS_FAILED(rv)) {
    LogMessage(aProxy, "download not allowed", nsIScriptError::errorFlag, rv);
    return rv;
  }

  nsCOMPtr<nsIStreamLoader> streamLoader;
  nsCOMPtr<nsILoadGroup> loadGroup(ps->GetDocument()->GetDocumentLoadGroup());

  nsCOMPtr<nsIChannel> channel;
  // get Content Security Policy from principal to pass into channel
  nsCOMPtr<nsIChannelPolicy> channelPolicy;
  nsCOMPtr<nsIContentSecurityPolicy> csp;
  rv = principal->GetCsp(getter_AddRefs(csp));
  NS_ENSURE_SUCCESS(rv, rv);
  if (csp) {
    channelPolicy = do_CreateInstance("@mozilla.org/nschannelpolicy;1");
    channelPolicy->SetContentSecurityPolicy(csp);
    channelPolicy->SetLoadType(nsIContentPolicy::TYPE_FONT);
  }
  rv = NS_NewChannel(getter_AddRefs(channel),
                     aFontFaceSrc->mURI,
                     nsnull,
                     loadGroup,
                     nsnull,
                     nsIRequest::LOAD_NORMAL,
                     channelPolicy);

  NS_ENSURE_SUCCESS(rv, rv);

  nsRefPtr<nsFontFaceLoader> fontLoader =
    new nsFontFaceLoader(aProxy, aFontFaceSrc->mURI, this, channel);

  if (!fontLoader)
    return NS_ERROR_OUT_OF_MEMORY;

#ifdef PR_LOGGING
  if (LOG_ENABLED()) {
    nsCAutoString fontURI, referrerURI;
    aFontFaceSrc->mURI->GetSpec(fontURI);
    if (aFontFaceSrc->mReferrer)
      aFontFaceSrc->mReferrer->GetSpec(referrerURI);
    LOG(("fontdownloader (%p) download start - font uri: (%s) "
         "referrer uri: (%s)\n", 
         fontLoader.get(), fontURI.get(), referrerURI.get()));
  }
#endif  

  nsCOMPtr<nsIHttpChannel> httpChannel(do_QueryInterface(channel));
  if (httpChannel)
    httpChannel->SetReferrer(aFontFaceSrc->mReferrer);
  rv = NS_NewStreamLoader(getter_AddRefs(streamLoader), fontLoader);
  NS_ENSURE_SUCCESS(rv, rv);
  
  PRBool inherits = PR_FALSE;
  rv = NS_URIChainHasFlags(aFontFaceSrc->mURI,
                           nsIProtocolHandler::URI_INHERITS_SECURITY_CONTEXT,
                           &inherits);
  if (NS_SUCCEEDED(rv) && inherits) {
    // allow data, javascript, etc URI's
    rv = channel->AsyncOpen(streamLoader, nsnull);
  } else {
    nsCOMPtr<nsIStreamListener> listener =
      new nsCORSListenerProxy(streamLoader, principal, channel, 
                              PR_FALSE, &rv);
    if (NS_FAILED(rv)) {
      fontLoader->DropChannel();  // explicitly need to break ref cycle
    }
    NS_ENSURE_TRUE(listener, NS_ERROR_OUT_OF_MEMORY);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = channel->AsyncOpen(listener, nsnull);
  }

  if (NS_SUCCEEDED(rv)) {
    mLoaders.PutEntry(fontLoader);
    fontLoader->StartedLoading(streamLoader);
  }

  return rv;
}

PRBool
nsUserFontSet::UpdateRules(const nsTArray<nsFontFaceRuleContainer>& aRules)
{
  PRBool modified = PR_FALSE;

  // destroy any current loaders, as the entries they refer to
  // may be about to get replaced
  if (mLoaders.Count() > 0) {
    modified = PR_TRUE; // trigger reflow so that any necessary downloads
                        // will be reinitiated
  }
  mLoaders.EnumerateEntries(DestroyIterator, nsnull);

  nsTArray<FontFaceRuleRecord> oldRules;
  mRules.SwapElements(oldRules);

  // destroy the font family records; we need to re-create them
  // because we might end up with faces in a different order,
  // even if they're the same font entries as before
  mFontFamilies.Clear();

  for (PRUint32 i = 0, i_end = aRules.Length(); i < i_end; ++i) {
    // insert each rule into our list, migrating old font entries if possible
    // rather than creating new ones; set  modified  to true if we detect
    // that rule ordering has changed, or if a new entry is created
    InsertRule(aRules[i].mRule, aRules[i].mSheetType, oldRules, modified);
  }

  // if any rules are left in the old list, note that the set has changed
  if (oldRules.Length() > 0) {
    modified = PR_TRUE;
  }

  if (modified) {
    IncrementGeneration();
  }

  return modified;
}

void
nsUserFontSet::InsertRule(nsCSSFontFaceRule *aRule, PRUint8 aSheetType,
                          nsTArray<FontFaceRuleRecord>& aOldRules,
                          PRBool& aFontSetModified)
{
  NS_ABORT_IF_FALSE(aRule->GetType() == mozilla::css::Rule::FONT_FACE_RULE,
                    "InsertRule passed a non-fontface CSS rule");

  // set up family name
  nsAutoString fontfamily;
  nsCSSValue val;
  PRUint32 unit;

  aRule->GetDesc(eCSSFontDesc_Family, val);
  unit = val.GetUnit();
  if (unit == eCSSUnit_String) {
    val.GetStringValue(fontfamily);
  } else {
    NS_ASSERTION(unit == eCSSUnit_Null,
                 "@font-face family name has unexpected unit");
  }
  if (fontfamily.IsEmpty()) {
    // If there is no family name, this rule cannot contribute a
    // usable font, so there is no point in processing it further.
    return;
  }

  // first, we check in oldRules; if the rule exists there, just move it
  // to the new rule list, and put the entry into the appropriate family
  for (PRUint32 i = 0; i < aOldRules.Length(); ++i) {
    const FontFaceRuleRecord& ruleRec = aOldRules[i];
    if (ruleRec.mContainer.mRule == aRule &&
        ruleRec.mContainer.mSheetType == aSheetType) {
      AddFontFace(fontfamily, ruleRec.mFontEntry);
      mRules.AppendElement(ruleRec);
      aOldRules.RemoveElementAt(i);
      // note the set has been modified if an old rule was skipped to find
      // this one - something has been dropped, or ordering changed
      if (i > 0) {
        aFontSetModified = PR_TRUE;
      }
      return;
    }
  }

  // this is a new rule:

  PRUint32 weight = NS_STYLE_FONT_WEIGHT_NORMAL;
  PRUint32 stretch = NS_STYLE_FONT_STRETCH_NORMAL;
  PRUint32 italicStyle = FONT_STYLE_NORMAL;
  nsString featureSettings, languageOverride;

  // set up weight
  aRule->GetDesc(eCSSFontDesc_Weight, val);
  unit = val.GetUnit();
  if (unit == eCSSUnit_Integer || unit == eCSSUnit_Enumerated) {
    weight = val.GetIntValue();
  } else if (unit == eCSSUnit_Normal) {
    weight = NS_STYLE_FONT_WEIGHT_NORMAL;
  } else {
    NS_ASSERTION(unit == eCSSUnit_Null,
                 "@font-face weight has unexpected unit");
  }

  // set up stretch
  aRule->GetDesc(eCSSFontDesc_Stretch, val);
  unit = val.GetUnit();
  if (unit == eCSSUnit_Enumerated) {
    stretch = val.GetIntValue();
  } else if (unit == eCSSUnit_Normal) {
    stretch = NS_STYLE_FONT_STRETCH_NORMAL;
  } else {
    NS_ASSERTION(unit == eCSSUnit_Null,
                 "@font-face stretch has unexpected unit");
  }

  // set up font style
  aRule->GetDesc(eCSSFontDesc_Style, val);
  unit = val.GetUnit();
  if (unit == eCSSUnit_Enumerated) {
    italicStyle = val.GetIntValue();
  } else if (unit == eCSSUnit_Normal) {
    italicStyle = FONT_STYLE_NORMAL;
  } else {
    NS_ASSERTION(unit == eCSSUnit_Null,
                 "@font-face style has unexpected unit");
  }

  // set up font features
  aRule->GetDesc(eCSSFontDesc_FontFeatureSettings, val);
  unit = val.GetUnit();
  if (unit == eCSSUnit_Normal) {
    // empty feature string
  } else if (unit == eCSSUnit_String) {
    val.GetStringValue(featureSettings);
  } else {
    NS_ASSERTION(unit == eCSSUnit_Null,
                 "@font-face font-feature-settings has unexpected unit");
  }

  // set up font language override
  aRule->GetDesc(eCSSFontDesc_FontLanguageOverride, val);
  unit = val.GetUnit();
  if (unit == eCSSUnit_Normal) {
    // empty feature string
  } else if (unit == eCSSUnit_String) {
    val.GetStringValue(languageOverride);
  } else {
    NS_ASSERTION(unit == eCSSUnit_Null,
                 "@font-face font-language-override has unexpected unit");
  }

  // set up src array
  nsTArray<gfxFontFaceSrc> srcArray;

  aRule->GetDesc(eCSSFontDesc_Src, val);
  unit = val.GetUnit();
  if (unit == eCSSUnit_Array) {
    nsCSSValue::Array *srcArr = val.GetArrayValue();
    size_t numSrc = srcArr->Count();
    
    for (size_t i = 0; i < numSrc; i++) {
      val = srcArr->Item(i);
      unit = val.GetUnit();
      gfxFontFaceSrc *face = srcArray.AppendElements(1);
      if (!face)
        return;

      switch (unit) {

      case eCSSUnit_Local_Font:
        val.GetStringValue(face->mLocalName);
        face->mIsLocal = PR_TRUE;
        face->mURI = nsnull;
        face->mFormatFlags = 0;
        break;
      case eCSSUnit_URL:
        face->mIsLocal = PR_FALSE;
        face->mURI = val.GetURLValue();
        NS_ASSERTION(face->mURI, "null url in @font-face rule");
        face->mReferrer = val.GetURLStructValue()->mReferrer;
        face->mOriginPrincipal = val.GetURLStructValue()->mOriginPrincipal;
        NS_ASSERTION(face->mOriginPrincipal, "null origin principal in @font-face rule");

        // agent and user stylesheets are treated slightly differently,
        // the same-site origin check and access control headers are
        // enforced against the sheet principal rather than the document
        // principal to allow user stylesheets to include @font-face rules
        face->mUseOriginPrincipal = (aSheetType == nsStyleSet::eUserSheet ||
                                     aSheetType == nsStyleSet::eAgentSheet);

        face->mLocalName.Truncate();
        face->mFormatFlags = 0;
        while (i + 1 < numSrc && (val = srcArr->Item(i+1), 
                 val.GetUnit() == eCSSUnit_Font_Format)) {
          nsDependentString valueString(val.GetStringBufferValue());
          if (valueString.LowerCaseEqualsASCII("woff")) {
            face->mFormatFlags |= FLAG_FORMAT_WOFF;
          } else if (valueString.LowerCaseEqualsASCII("opentype")) {
            face->mFormatFlags |= FLAG_FORMAT_OPENTYPE;
          } else if (valueString.LowerCaseEqualsASCII("truetype")) {
            face->mFormatFlags |= FLAG_FORMAT_TRUETYPE;
          } else if (valueString.LowerCaseEqualsASCII("truetype-aat")) {
            face->mFormatFlags |= FLAG_FORMAT_TRUETYPE_AAT;
          } else if (valueString.LowerCaseEqualsASCII("embedded-opentype")) {
            face->mFormatFlags |= FLAG_FORMAT_EOT;
          } else if (valueString.LowerCaseEqualsASCII("svg")) {
            face->mFormatFlags |= FLAG_FORMAT_SVG;
          } else {
            // unknown format specified, mark to distinguish from the
            // case where no format hints are specified
            face->mFormatFlags |= FLAG_FORMAT_UNKNOWN;
          }
          i++;
        }
        break;
      default:
        NS_ASSERTION(unit == eCSSUnit_Local_Font || unit == eCSSUnit_URL,
                     "strange unit type in font-face src array");
        break;
      }
     }
  } else {
    NS_ASSERTION(unit == eCSSUnit_Null, "@font-face src has unexpected unit");
  }

  if (srcArray.Length() > 0) {
    FontFaceRuleRecord ruleRec;
    ruleRec.mContainer.mRule = aRule;
    ruleRec.mContainer.mSheetType = aSheetType;
    ruleRec.mFontEntry = AddFontFace(fontfamily, srcArray,
                                     weight, stretch, italicStyle,
                                     featureSettings, languageOverride);
    if (ruleRec.mFontEntry) {
      mRules.AppendElement(ruleRec);
    }
    // this was a new rule and fontEntry, so note that the set was modified
    aFontSetModified = PR_TRUE;
  }
}

void
nsUserFontSet::ReplaceFontEntry(gfxProxyFontEntry *aProxy,
                                gfxFontEntry *aFontEntry)
{
  for (PRUint32 i = 0; i < mRules.Length(); ++i) {
    if (mRules[i].mFontEntry == aProxy) {
      mRules[i].mFontEntry = aFontEntry;
      break;
    }
  }
  static_cast<gfxMixedFontFamily*>(aProxy->Family())->
    ReplaceFontEntry(aProxy, aFontEntry);
}

nsCSSFontFaceRule*
nsUserFontSet::FindRuleForEntry(gfxFontEntry *aFontEntry)
{
  for (PRUint32 i = 0; i < mRules.Length(); ++i) {
    if (mRules[i].mFontEntry == aFontEntry) {
      return mRules[i].mContainer.mRule;
    }
  }
  return nsnull;
}

nsresult
nsUserFontSet::LogMessage(gfxProxyFontEntry *aProxy,
                          const char        *aMessage,
                          PRUint32          aFlags,
                          nsresult          aStatus)
{
  nsCOMPtr<nsIConsoleService>
    console(do_GetService(NS_CONSOLESERVICE_CONTRACTID));
  if (!console) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  NS_ConvertUTF16toUTF8 familyName(aProxy->FamilyName());
  nsCAutoString fontURI;
  if (aProxy->mSrcList[aProxy->mSrcIndex].mURI) {
    aProxy->mSrcList[aProxy->mSrcIndex].mURI->GetSpec(fontURI);
  } else {
    fontURI.AppendLiteral("(invalid URI)");
  }

  char weightKeywordBuf[8]; // plenty to sprintf() a PRUint16
  const char *weightKeyword;
  const nsAFlatCString& weightKeywordString =
    nsCSSProps::ValueToKeyword(aProxy->Weight(),
                               nsCSSProps::kFontWeightKTable);
  if (weightKeywordString.Length() > 0) {
    weightKeyword = weightKeywordString.get();
  } else {
    sprintf(weightKeywordBuf, "%u", aProxy->Weight());
    weightKeyword = weightKeywordBuf;
  }

  nsPrintfCString
    msg(1024,
        "downloadable font: %s "
        "(font-family: \"%s\" style:%s weight:%s stretch:%s src index:%d)",
        aMessage,
        familyName.get(),
        aProxy->IsItalic() ? "italic" : "normal",
        weightKeyword,
        nsCSSProps::ValueToKeyword(aProxy->Stretch(),
                                   nsCSSProps::kFontStretchKTable).get(),
        aProxy->mSrcIndex);

  if (aStatus != 0) {
    msg.Append(": ");
    switch (aStatus) {
    case NS_ERROR_DOM_BAD_URI:
      msg.Append("bad URI or cross-site access not allowed");
      break;
    case NS_ERROR_CONTENT_BLOCKED:
      msg.Append("content blocked");
      break;
    default:
      msg.Append("status=");
      msg.AppendInt(aStatus);
      break;
    }
  }
  msg.Append("\nsource: ");
  msg.Append(fontURI);

#ifdef PR_LOGGING
  if (PR_LOG_TEST(sUserFontsLog, PR_LOG_DEBUG)) {
    PR_LOG(sUserFontsLog, PR_LOG_DEBUG,
           ("userfonts (%p) %s", this, msg.get()));
  }
#endif

  // try to give the user an indication of where the rule came from
  nsCSSFontFaceRule* rule = FindRuleForEntry(aProxy);
  nsString href;
  nsString text;
  nsresult rv;
  if (rule) {
    rv = rule->GetCssText(text);
    NS_ENSURE_SUCCESS(rv, rv);
    nsCOMPtr<nsIDOMCSSStyleSheet> sheet;
    rv = rule->GetParentStyleSheet(getter_AddRefs(sheet));
    NS_ENSURE_SUCCESS(rv, rv);
    rv = sheet->GetHref(href);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  nsCOMPtr<nsIScriptError2> scriptError =
    do_CreateInstance(NS_SCRIPTERROR_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  PRUint64 windowID = GetPresContext()->Document()->OuterWindowID();
  rv = scriptError->InitWithWindowID(NS_ConvertUTF8toUTF16(msg).get(),
                                     href.get(),   // file
                                     text.get(),   // src line
                                     0, 0,         // line & column number
                                     aFlags,       // flags
                                     "CSS Loader", // category (make separate?)
                                     windowID);
  if (NS_SUCCEEDED(rv)){
    nsCOMPtr<nsIScriptError> logError = do_QueryInterface(scriptError);
    if (logError) {
      console->LogMessage(logError);
    }
  }

  return NS_OK;
}
