#ifndef tgputtylibcbclassH
#define tgputtylibcbclassH

#include <string>
#include <tgputtylib.hpp>


typedef void (__closure *TOnMessage)(const char *Msg, const bool isstderr);
typedef bool (__closure *TOnProgress)(const __int64 bytescopied, const bool isupload);
typedef bool (__closure *TOnListing)(const struct fxp_names *names);
typedef bool (__closure *TOnGetInput)(char *linebuf,const int maxchars);
typedef bool (__closure *TOnVerifyHostKey)
                   (const char * host, const int port,
                    const char * fingerprint, const int verificationstatus, bool &storehostkey);

static const int MinimumLibraryBuildNum = 0x1;
static const short cDummyClearedErrorCode = short(-1000);  // this error code means there was no real error code

struct TTGPuttySFTPException : public std::exception
{
   std::string s;
   TTGPuttySFTPException(std::string ans) : s(ans) {}
   ~TTGPuttySFTPException() throw () {}
   const char* what() const throw() { return s.c_str(); }
};

class TTGPuttySFTP
{
  public:
	TTGLibraryContext Fcontext;
	bool FVerbose;
	std::string FHostName;
	std::string FUserName;
	std::string FPassword;
	std::string FKeyPassword;
	int FPort;
	TOnMessage FOnMessage;
	TOnProgress FOnProgress;
	TOnListing FOnListing;
	TOnGetInput FOnGetInput;
	TOnVerifyHostKey FOnVerifyHostKey;
	//System::Classes::TStream* FUploadStream;
	//System::Classes::TStream* FDownloadStream;
	bool FConnected;
	int FPasswordAttempts;

	std::string FLastMessages;

	char *GetHomeDir();
	char *GetWorkDir();
	void SetVerbose(const bool Value);
	void SetKeyfile(const char *Value);
	void SetHostName(const char *Value);
	void SetPassword(const char *Value);
	void SetUserName(const char *Value);
	void SetKeyPassword(const char *Value);
	char *GetLibVersion();
	int GetErrorCode();
	const char *GetErrorMessage();

	TTGPuttySFTP(const bool verbose);
	virtual ~TTGPuttySFTP();
    void ClearStatus();
	std::string MakePSFTPErrorMsg(const char *where);
	void Connect();
	void Disconnect();
	void ChangeDir(const char *ADirectory);
	void MakeDir(const char *ADirectory);
	void RemoveDir(const char *ADirectory);
	void ListDir(const char *ADirectory);
	void GetStat(const char *AFileName, Tfxp_attrs *Attrs);
	void SetStat(const char *AFileName, struct fxp_attrs *Attrs);
	void SetModifiedDate(const char *AFileName, const __int64 unixtime);
	void SetFileSize(const char *AFileName, const __int64 ASize);
	void Move(const char *AFromName, const char *AToName);
	void DeleteFile(const char *AName);

	void UploadFile(const char *ALocalFilename, const char *ARemoteFilename, const bool anAppend);
	void DownloadFile(const char *ARemoteFilename, const char *ALocalFilename, const bool anAppend);

	//void UploadStream(const char *ARemoteFilename, System::Classes::TStream* const AStream, const bool anAppend);
	//void DownloadStream(const char *ARemoteFilename, System::Classes::TStream* const AStream, const bool anAppend);

	void * OpenFile(const char *apathname, const int anopenflags, const Pfxp_attrs attrs);
	int CloseFile(struct fxp_handle * &fh);
	void * xfer_upload_init(struct fxp_handle *fh, const unsigned __int64 offset);
	bool xfer_upload_ready(struct fxp_xfer *xfer);
	void xfer_upload_data(struct fxp_xfer *xfer, char *buffer, const int len, const unsigned __int64 anoffset);
	bool xfer_ensuredone(struct fxp_xfer *xfer);
	bool xfer_done(struct fxp_xfer *xfer);
	void xfer_cleanup(struct fxp_xfer *xfer);

	__property char *HostName = {read=FHostName, write=SetHostName};
	__property char *UserName = {read=FUserName, write=SetUserName};
	__property int Port = {read=FPort, write=FPort};
	__property char *Password = {read=FPassword, write=SetPassword};
	__property char *KeyPassword = {read=FKeyPassword, write=SetKeyPassword};
	__property char *HomeDir = {read=GetHomeDir};
	__property char *WorkDir = {read=GetWorkDir};
	__property char *LibVersion = {read=GetLibVersion};
	__property bool Connected = {read=FConnected};
	__property bool Verbose = {read=FVerbose, write=SetVerbose};
	__property char *Keyfile = {write=SetKeyfile};
	__property std::string LastMessages = {read=FLastMessages, write=FLastMessages};
	__property int ErrorCode = {read=GetErrorCode};
	__property const char *ErrorMessage = {read=GetErrorMessage};
	__property TOnMessage OnMessage = {read=FOnMessage, write=FOnMessage};
	__property TOnProgress OnProgress = {read=FOnProgress, write=FOnProgress};
	__property TOnListing OnListing = {read=FOnListing, write=FOnListing};
	__property TOnGetInput OnGetInput = {read=FOnGetInput, write=FOnGetInput};
	__property TOnVerifyHostKey OnVerifyHostKey = {read=FOnVerifyHostKey, write=FOnVerifyHostKey};
};
#endif
