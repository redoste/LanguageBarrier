#include <windows.h>

#include "Game.h"
#include "SigScan.h"
#include "LanguageBarrier.h"
#include "TwipoSynchroRNE.h"

namespace lb {
namespace rne {
namespace twipoSynchro {

HANDLE tweepMutex = NULL;
HANDLE game2server = NULL, server2game = NULL;

#define CONDITION(message, condition, function, withError, error)  \
  do {                                                             \
    if (!(condition)) {                                            \
      function(message, __LINE__, withError, error);               \
      return false;                                                \
    }                                                              \
  } while (0)

#define CHECK_INIT_WIN32(message, condition) CONDITION(message, condition, failInit, true, GetLastError())
#define CHECK_INIT_ERR(message, condition, error) CONDITION(message, condition, failInit, true, error)
#define CHECK_INIT(message, condition) CONDITION(message, condition, failInit, false, 0)
#define ASSERT_INIT(condition) CHECK_INIT(#condition, condition)

#define CHECK_WIN32(message, condition) CONDITION(message, condition, failRuntime, true, GetLastError())
#define CHECK_ERR(message, condition, error) CONDITION(message, condition, failRuntime, true, error)
#define CHECK(message, condition) CONDITION(message, condition, failRuntime, false, 0)
#define ASSERT(condition) CHECK(#condition, condition)

static void fail(const wchar_t* messageBoxMessage, const char* logPrefix, const char* message, int line, bool withError, int error) {
  std::stringstream logstr;
  logstr << logPrefix << " : " << __FILE__ << ":" << line << " : " << message;
  if (withError) {
    logstr << " : " << error;
  }
  LanguageBarrierLog(logstr.str());
  MessageBox(NULL, messageBoxMessage, L"twipo-synchro", MB_ICONERROR);
}

static void failInit(const char* message, int line, bool with_error, int error) {
  fail(L"Unable to start twipo-synchro.\nCheck LanguageBarrier log for more details.", "twipo-synchro start failed", message, line, with_error, error);
}

static void failRuntime(const char *message, int line, bool with_error, int error) {
  fail(L"An error occured in twipo-synchro.\nCheck LanguageBarrier log for more details.", "twipo-synchro runtime fail", message, line, with_error, error);
  CloseHandle(game2server);
  CloseHandle(server2game);
  game2server = NULL;
  server2game = NULL;
}

enum ScrWorkEnum {
  SW_TWIPOTOTALANS = 2010,
  SW_TWIPOCURTW = 6459,
  SW_TWIPOMODE = 6460,
  SW_TWIPOCURTAB = 6461,
  SW_TWIPOCURREP = 6462,
};

struct tweep_tab_entry_t {
  uint32_t tweepId;
  uint32_t postSate;
};
static tweep_tab_entry_t **TwipoTweepsInTabs = NULL;
static int *TwipoTweepCountInTabs = NULL;

struct tweep_t {
  uint32_t authorId;
  uint32_t repliesId[3];
  uint32_t stringId;
};
static tweep_t *TwipoTweeps = NULL;

static int *TwipoTweepCanBeReplied = NULL;
static int *TwipoTweepRepliedForAchievements = NULL;
static int *TwipoTweepsRepliesForFlag = NULL;
static int *TwipoAuthorTabs = NULL;
static int *TwipoAuthorNames = NULL;

typedef int(__cdecl *TwipoGetPFPProc)(int);
static TwipoGetPFPProc gameExeTwipoGetPFP = NULL;
typedef uint8_t *(__cdecl *GetSC3StringByIDProc)(int, int);
static GetSC3StringByIDProc gameExeGetSC3StringByID = NULL;
typedef void(__cdecl *AchievementsAndFlagForRoutesAndSciADVCastTwipoProc)();
static AchievementsAndFlagForRoutesAndSciADVCastTwipoProc gameExeAchievementsAndFlagForRoutesAndSciADVCastTwipo = NULL;

static bool startServer(std::string listenAddress) {
  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = NULL;

  HANDLE serverStdoutR, serverStdoutW;
  CHECK_INIT_WIN32("CreatePipe stdout", CreatePipe(&serverStdoutR, &serverStdoutW, &sa, 0));
  CHECK_INIT_WIN32("SetHandleInformation stdout", SetHandleInformation(serverStdoutR, HANDLE_FLAG_INHERIT, 0));

  HANDLE serverStdinR, serverStdinW;
  CHECK_INIT_WIN32("CreatePipe stdin", CreatePipe(&serverStdinR, &serverStdinW, &sa, 0));
  CHECK_INIT_WIN32("SetHandleInformation stdin", SetHandleInformation(serverStdinW, HANDLE_FLAG_INHERIT, 0));

  AllocConsole();

  PROCESS_INFORMATION procInfo = {};
  STARTUPINFO startInfo = {};
  startInfo.cb = sizeof(startInfo);
  startInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  startInfo.hStdOutput = serverStdoutW;
  startInfo.hStdInput = serverStdinR;
  startInfo.dwFlags = STARTF_USESTDHANDLES;

  std::wstring commandLine = L".\\twipo-synchro\\twipo-synchro.exe " + std::wstring(listenAddress.begin(), listenAddress.end());
  CHECK_INIT_WIN32("CreateProcess", CreateProcess(NULL, (LPWSTR)commandLine.c_str(), NULL, NULL, TRUE, 0, NULL, NULL, &startInfo, &procInfo));

  CloseHandle(serverStdoutW);
  CloseHandle(serverStdinR);

  server2game = serverStdoutR;
  game2server = serverStdinW;
  return true;
}

static bool sendAddTweep(int tweepId) {
  tweep_t* tweep = &TwipoTweeps[tweepId];
  int tab = TwipoAuthorTabs[tweep->authorId];
  int pfp = gameExeTwipoGetPFP(tweep->authorId);

  bool canBeReplied = true;
  const size_t repliesAmount = sizeof(tweep->repliesId) / sizeof(tweep->repliesId[0]);
  for (size_t i = 0; i < repliesAmount; i++) {
    canBeReplied &= tweep->repliesId[i] != 0xffff;
  }

#pragma pack(push, 1)
  struct {
    uint32_t messageType;
    uint32_t tweepId;
    uint16_t tab;
    uint16_t pfp;
    uint16_t differentDay;
    uint16_t repliesAmount;
  } messageHeader = {
    0x54574550,
    (uint32_t)tweepId,
    (uint16_t)tab,
    (uint16_t)pfp,
    0,
    (uint16_t)(canBeReplied ? repliesAmount : 0),
  };
  // TODO : handle different day
#pragma pack(pop)
  DWORD writtenBytes;
  CHECK_WIN32("WriteFile addTweep messageHeader", WriteFile(game2server, &messageHeader, sizeof(messageHeader), &writtenBytes, NULL) && writtenBytes == sizeof(messageHeader));

  uint8_t *authorUsername = gameExeGetSC3StringByID(0xb, TwipoAuthorNames[tweep->authorId]);
  ASSERT(authorUsername);
  uint8_t *tweepText = gameExeGetSC3StringByID(0xb, tweep->stringId);
  ASSERT(tweepText);

  size_t authorUsernameLen = 0;
  uint8_t *iter = authorUsername;
  // We look through 2 strings : username and realname
  for (int loops = 0; loops < 2; loops++) {
    for (; *iter != 0xff; iter++) {
      authorUsernameLen++;
    }
    iter++;
    authorUsernameLen++;
  }

  size_t tweepTextLen = 0;
  for (uint8_t *iter = tweepText; *iter != 0xff; iter++) {
    tweepTextLen++;
  }
  tweepTextLen++;

  CHECK_WIN32("WriteFile addTweep authorUsername", WriteFile(game2server, authorUsername, authorUsernameLen, &writtenBytes, NULL) && writtenBytes == authorUsernameLen);
  CHECK_WIN32("WriteFile addTweep tweepText", WriteFile(game2server, tweepText, tweepTextLen, &writtenBytes, NULL) && writtenBytes == tweepTextLen);

  if (canBeReplied) {
    for (size_t i = 0; i < repliesAmount; i++) {
      tweep_t *replyTweep = &TwipoTweeps[tweep->repliesId[i]];
      uint8_t *replyText = gameExeGetSC3StringByID(0xb, replyTweep->stringId);
      size_t replyTextLen = 0;
      for (uint8_t *iter = replyText; *iter != 0xff; iter++) {
        replyTextLen++;
      }
      replyTextLen++;

      CHECK_WIN32("WriteFile addTweep replyText", WriteFile(game2server, replyText, replyTextLen, &writtenBytes, NULL) && writtenBytes == replyTextLen);
    }
  }
  return true;
}

static bool sendSetReplyPossible(int tweepId) {
#pragma pack(push, 1)
  struct {
    uint32_t messageType;
    uint32_t tweepId;
    uint16_t possible;
  } message = {
      0x53545250,
      (uint32_t)tweepId,
      (uint16_t)TwipoTweepCanBeReplied[tweepId],
  };
#pragma pack(pop)
  DWORD writtenBytes;
  CHECK_WIN32("WriteFile sendSetReplyPossible", WriteFile(game2server, &message, sizeof(message), &writtenBytes, NULL) && writtenBytes == sizeof(message));
  return true;
}

static bool sendClearTweeps() {
  uint32_t clearTweeps = 0x434c4541;
  DWORD writtenBytes;
  CHECK_WIN32("WriteFile sendClearTweeps", WriteFile(game2server, &clearTweeps, sizeof(clearTweeps), &writtenBytes, NULL) && writtenBytes == sizeof(clearTweeps));
  return true;
}

typedef void(__cdecl *AddTweepProc)(int);
static AddTweepProc gameExeAddTweep = NULL;
static AddTweepProc gameExeAddTweepReal = NULL;
void __cdecl addTweepHook(int tweepId) {
  if (!game2server) {
    gameExeAddTweepReal(tweepId);
    return;
  }

  if (WaitForSingleObject(tweepMutex, INFINITE) != WAIT_OBJECT_0) {
    failRuntime("WaitForSingleObject addTweepHook", __LINE__, true, GetLastError());
    gameExeAddTweepReal(tweepId);
    return;
  }

  bool shouldEmpty = true;
  for (size_t i = 0; i < 4; i++) {
    shouldEmpty &= TwipoTweepCountInTabs[i] == 0;
  }
  bool ret = true;
  if (shouldEmpty) {
    ret = sendClearTweeps();
  }
  if (ret) {
    sendAddTweep(tweepId);
  }

  gameExeAddTweepReal(tweepId);
  ReleaseMutex(tweepMutex);
}

typedef void *(__cdecl *LoadTwipoFromSaveProc)(void *);
static LoadTwipoFromSaveProc gameExeLoadTwipoFromSave = NULL;
static LoadTwipoFromSaveProc gameExeLoadTwipoFromSaveReal = NULL;
void *__cdecl loadTwipoFromSaveHook(void *save) {
  if (!game2server) {
    return gameExeLoadTwipoFromSaveReal(save);
  }
  if (WaitForSingleObject(tweepMutex, INFINITE) != WAIT_OBJECT_0) {
    fail_runtime("WaitForSingleObject loadTwipoFromSaveHook", __LINE__, true, GetLastError());
    return gameExeLoadTwipoFromSaveReal(save);
  }

  void *ret = gameExeLoadTwipoFromSaveReal(save);

  bool no_error = sendClearTweeps();
  for (int tab = 0; tab < 4 && no_error; tab++) {
    for (int tweep = 0; tweep < TwipoTweepCountInTabs[tab] && no_error; tweep++) {
      int tweepId = TwipoTweepsInTabs[tab][tweep].tweepId;
      no_error &= sendAddTweep(tweepId);
      no_error &= sendSetReplyPossible(tweepId);
    }
  }

  ReleaseMutex(tweepMutex);
  return ret;
}

typedef void(__cdecl *MarkTweepReplyPossibleProc)(int);
static MarkTweepReplyPossibleProc gameExeMarkTweepReplyPossible = NULL;
static MarkTweepReplyPossibleProc gameExeMarkTweepReplyPossibleReal = NULL;
static MarkTweepReplyPossibleProc gameExeMarkTweepReplyNotPossible = NULL;
static MarkTweepReplyPossibleProc gameExeMarkTweepReplyNotPossibleReal = NULL;
void __cdecl markTweepReplyPossibleHook(int tweepId) {
  if (!game2server) {
    gameExeMarkTweepReplyPossibleReal(tweepId);
    return;
  }
  if (WaitForSingleObject(tweepMutex, INFINITE) != WAIT_OBJECT_0) {
    failRuntime("WaitForSingleObject markTweepReplyPossibleHook", __LINE__, true, GetLastError());
    gameExeMarkTweepReplyPossibleReal(tweepId);
    return;
  }
  gameExeMarkTweepReplyPossibleReal(tweepId);
  sendSetReplyPossible(tweepId);
  ReleaseMutex(tweepMutex);
}
void __cdecl markTweepReplyNotPossibleHook(int tweepId) {
  if (!game2server) {
    gameExeMarkTweepReplyNotPossibleReal(tweepId);
    return;
  }
  if (WaitForSingleObject(tweepMutex, INFINITE) != WAIT_OBJECT_0) {
    failRuntime("WaitForSingleObject markTweepReplyNotPossibleHook", __LINE__, true, GetLastError());
    gameExeMarkTweepReplyNotPossibleReal(tweepId);
    return;
  }
  gameExeMarkTweepReplyNotPossibleReal(tweepId);
  sendSetReplyPossible(tweepId);
  ReleaseMutex(tweepMutex);
}

typedef void(__cdecl *UpdateAchievementsProc)(int);
static UpdateAchievementsProc gameExeUpdateAchievements = NULL;
static UpdateAchievementsProc gameExeUpdateAchievementsReal = NULL;
static const int ACHIEVEMENTS_TWEEPS = 8;
void __cdecl updateAchievementsHook(int achievementType) {
  if (game2server && achievementType == ACHIEVEMENTS_TWEEPS) {
    if (WaitForSingleObject(tweepMutex, INFINITE) != WAIT_OBJECT_0) {
      failRuntime("WaitForSingleObject updateAchievementsHook", __LINE__, true, GetLastError());
    } else {
      sendSetReplyPossible(TwipoTweepsInTabs[gameExeScrWork[SW_TWIPOCURTAB]][gameExeScrWork[SW_TWIPOCURTW]].tweepId);
      ReleaseMutex(tweepMutex);
    }
  }
  gameExeUpdateAchievementsReal(achievementType);
}

DWORD WINAPI replyThread(LPVOID lpParameter) {
  while (true) {
    if (!server2game) {
      return 0;
    }

#pragma pack(push, 1)
    struct {
      uint32_t messageType;
      uint32_t tweepId;
      uint32_t replyId;
    } message;
#pragma pack(pop)
    size_t read = 0;
    bool ret = true;
    while (ret && read < sizeof(message)) {
      DWORD currentRead = 0;
      ret = ReadFile(server2game, ((uint8_t *)&message + read), sizeof(message) - read, &currentRead, 0);
      read += currentRead;
      ret &= currentRead > 0;
    }
    CHECK_WIN32("ReadFile replyThread", ret);

    ASSERT(message.messageType == 0x594c5052);

    ASSERT(message.replyId < 3);
    int replyTweepId = TwipoTweeps[message.tweepId].repliesId[message.replyId];
    ASSERT(TwipoTweepCanBeReplied[message.tweepId] && replyTweepId != 0xffff);

    CHECK_WIN32("WaitForSingleObject replyThread", WaitForSingleObject(tweepMutex, INFINITE) == WAIT_OBJECT_0);
    gameExeAddTweepReal(replyTweepId);
    bool noError = sendAddTweep(replyTweepId, gameExeScrWork[LR_DATE]);

    TwipoTweepCanBeReplied[message.tweepId] = 0;
    if (noError) {
      sendSetReplyPossible(message.tweepId);
    }
    TwipoTweepRepliedForAchievements[message.tweepId] = 1;
    gameExeScrWork[SW_TWIPOTOTALANS]++;
    gameExeUpdateAchievementsReal(ACHIEVEMENTS_TWEEPS);

    TwipoTweepsRepliesForFlag[replyTweepId] = 1;
    gameExeAchievementsAndFlagForRoutesAndSciADVCastTwipo();

    ReleaseMutex(tweepMutex);
  }
}

typedef uint32_t(__cdecl* criFsStdio_OpenFile_proc)(uint32_t bndr, char *fname, char *mode);
typedef int64_t (__cdecl* criFsStdio_GetFileSize_proc)(uint32_t stdhn);
typedef int64_t (__cdecl* criFsStdio_ReadFile_proc)(uint32_t stdhn, int64_t rsize, void* buf, int64_t bsize);
typedef int (__cdecl* criFsStdio_CloseFile_proc)(uint32_t stdhn);

static bool sendArChip3() {
  mgsVFSObject *gameExeFileObjects = (mgsVFSObject *)sigScan("game", "useOfFileObjects");
  ASSERT_INIT(gameExeFileObjects);
  mgsVFSObject *systemVFS = &gameExeFileObjects[2];
  ASSERT_INIT(strcmp(systemVFS->archiveName, "SYSTEM") == 0);

  criFsStdio_OpenFile_proc criFsStdio_OpenFile = (criFsStdio_OpenFile_proc) sigScan("game", "criFsStdio_OpenFile");
  ASSERT_INIT(criFsStdio_OpenFile);
  criFsStdio_GetFileSize_proc criFsStdio_GetFileSize = (criFsStdio_GetFileSize_proc)sigScan("game", "criFsStdio_GetFileSize");
  ASSERT_INIT(criFsStdio_GetFileSize);
  criFsStdio_ReadFile_proc criFsStdio_ReadFile = (criFsStdio_ReadFile_proc)sigScan("game", "criFsStdio_ReadFile");
  ASSERT_INIT(criFsStdio_ReadFile);
  criFsStdio_CloseFile_proc criFsStdio_CloseFile = (criFsStdio_CloseFile_proc)sigScan("game", "criFsStdio_CloseFile");
  ASSERT_INIT(criFsStdio_CloseFile);

  uint32_t arChip3hn = criFsStdio_OpenFile(systemVFS->bndrhn, "AR_CHIP3_en.png", "r");
  ASSERT_INIT(arChip3hn);
  size_t arChip3Size = (size_t) criFsStdio_GetFileSize(arChip3hn);
  static_assert(sizeof(arChip3Size) == sizeof(uint32_t));

  uint8_t *arChip3 = new uint8_t[arChip3Size];
  int64_t readSize = criFsStdio_ReadFile(arChip3hn, arChip3Size, arChip3, arChip3Size);
  ASSERT_INIT(readSize == arChip3Size);

  int err = criFsStdio_CloseFile(arChip3hn);
  CHECK_INIT_ERR("criFsStdio_CloseFile(arChip3hn)", err == 0, err);

  DWORD writtenBytes;
  CHECK_INIT_WIN32("WriteFile arChip3Size", WriteFile(game2server, &arChip3Size, sizeof(arChip3Size), &writtenBytes, NULL) && writtenBytes == sizeof(arChip3Size));
  CHECK_INIT_WIN32("WriteFile arChip3", WriteFile(game2server, arChip3, arChip3Size, &writtenBytes, NULL) && writtenBytes == arChip3Size);

  delete arChip3;
  return true;
}

bool twipoSynchroInit(std::string listenAddress) {
  gameExeTwipoGetPFP = (TwipoGetPFPProc)sigScan("game", "TwipoGetPFP");
  ASSERT_INIT(gameExeTwipoGetPFP);
  gameExeGetSC3StringByID = (GetSC3StringByIDProc)sigScan("game", "GetSC3StringByID");
  ASSERT_INIT(gameExeGetSC3StringByID);
  gameExeAchievementsAndFlagForRoutesAndSciADVCastTwipo = (AchievementsAndFlagForRoutesAndSciADVCastTwipoProc)sigScan("game", "AchievementsAndFlagForRoutesAndSciADVCastTwipo");
  ASSERT_INIT(gameExeAchievementsAndFlagForRoutesAndSciADVCastTwipo);

  TwipoTweepCountInTabs = (int *)sigScan("game", "useOfTwipoTweepCountInTabs");
  ASSERT_INIT(TwipoTweepCountInTabs);
  TwipoTweepsInTabs = (tweep_tab_entry_t **)sigScan("game", "useOfTwipoDataPointers");
  ASSERT_INIT(TwipoTweepsInTabs);
  TwipoTweeps = (tweep_t *)(sigScan("game", "useOfTwipoData1") - 4);
  ASSERT_INIT(TwipoTweeps);
  TwipoTweepCanBeReplied = (int *)sigScan("game", "useOfTwipoData2");
  ASSERT_INIT(TwipoTweepCanBeReplied);
  TwipoTweepRepliedForAchievements = (int *)sigScan("game", "useOfTwipoTweepRepliedForAchievements");
  ASSERT_INIT(TwipoTweepRepliedForAchievements);
  TwipoTweepsRepliesForFlag = (int *)sigScan("game", "useOfTwipoTweepsRepliesForFlag");
  ASSERT_INIT(TwipoTweepsRepliesForFlag);
  TwipoAuthorTabs = (int *)sigScan("game", "useOfTwipoAuthorTabs");
  ASSERT_INIT(TwipoAuthorTabs);
  TwipoAuthorNames = (int *)sigScan("game", "useOfTwipoAuthorNames");
  ASSERT_INIT(TwipoAuthorNames);

  ASSERT_INIT(scanCreateEnableHook("game", "AddTweep", (uintptr_t *)&gameExeAddTweep, (LPVOID *)&addTweepHook, (LPVOID *)&gameExeAddTweepReal));
  ASSERT_INIT(scanCreateEnableHook("game", "LoadTwipoFromSave", (uintptr_t *)&gameExeLoadTwipoFromSave, (LPVOID *)&loadTwipoFromSaveHook, (LPVOID *)&gameExeLoadTwipoFromSaveReal));
  ASSERT_INIT(scanCreateEnableHook("game", "MarkTweepReplyPossible", (uintptr_t *)&gameExeMarkTweepReplyPossible, (LPVOID *)&markTweepReplyPossibleHook, (LPVOID *)&gameExeMarkTweepReplyPossibleReal));
  ASSERT_INIT(scanCreateEnableHook("game", "MarkTweepReplyNotPossible", (uintptr_t *)&gameExeMarkTweepReplyNotPossible, (LPVOID *)&markTweepReplyNotPossibleHook, (LPVOID *)&gameExeMarkTweepReplyNotPossibleReal));
  ASSERT_INIT(scanCreateEnableHook("game", "UpdateAchievements", (uintptr_t *)&gameExeUpdateAchievements, (LPVOID *)&updateAchievementsHook, (LPVOID *)&gameExeUpdateAchievementsReal));

  if (!startServer(listenAddress) || !sendArChip3()) {
    return false;
  }

  tweepMutex = CreateMutex(NULL, FALSE, NULL);
  ASSERT_INIT(tweepMutex);
  CHECK_INIT_WIN32("CreateThread replyThread", CreateThread(NULL, 0, &replyThread, NULL, 0, NULL));

  LanguageBarrierLog("twipo-synchro started successfully");
  return true;
}

}  // namespace twipoSynchro
}  // namespace rne
}  // namespace lb
