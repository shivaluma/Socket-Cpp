// Socket_Winsock.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include "pch.h"
#include "framework.h"
#include "SocketT.h"
#include <afxsock.h>
#include <fstream>
#include <direct.h>
#include <string>
#include <sstream>
#include <chrono>
#include <ctime>   
#include <map>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

// The one and only application object

CWinApp theApp;

using namespace std;

#define _WINSOCK_DEPRECATED_NO_WARNINGS
#pragma warning(disable:4996)


typedef SOCKET sock;

#define bufsize 200000
#define cacheTTL 3600 //cache song 1h
#define MAXTIMETORETRY 4

UINT ProxyServer(void* Iparam);
void Init_Server();
bool isGETorPOSTmethod(char* req);
bool GetDomainName(char* request, char* dname, int& port);
bool isHTTPs(char* req);
bool isHTTP10(char* request);
int getLengthOfFile(string);
int getFileSize(const std::string& fileName);
string getCacheLink(char* request);
string convertFileName(string linktemp);
bool checkCacheControl(string);
void setMapBlackList();

CWinThread* mainThread;
CWinThread* subThread;

map<string, int> domainblacklist;

int main()
{
	int nRetCode = 0;

	HMODULE hModule = ::GetModuleHandle(nullptr);

	if (hModule != nullptr)
	{
		// initialize MFC and print and error on failure
		if (!AfxWinInit(hModule, nullptr, ::GetCommandLine(), 0))
		{
			// TODO: code your application's behavior here.
			wprintf(L"Fatal Error: MFC initialization failed\n");
			nRetCode = 1;
		}
		else
		{
			setMapBlackList();
			Init_Server();
		}
	}
	else
	{
		// TODO: change error code to suit your needs
		wprintf(L"Fatal Error: GetModuleHandle failed\n");
		nRetCode = 1;
	}

	return nRetCode;
}

// khởi tạo map blacklist trước khi thực hiện khởi tạo proxy server.
void setMapBlackList() {
	//Check blacklist
	fstream blacklist("blacklist.conf", ios::in);
	if (blacklist.is_open())
	{
		string tmp;
		while (!blacklist.eof())
		{
			blacklist >> tmp;
			// thuc hien blacklist truong hop www.domain
			// vi example.com va www.example.com cho ket qua phan giai ip nhu nhau.
			if (tmp.find("www.") == string::npos) {
				tmp = "www." + tmp;
				domainblacklist[tmp]++;
			}
			else {
				domainblacklist[tmp]++;
			}
		}
	}
}


void Init_Server()
{
	sockaddr_in listenaddr;
	sock listensock = INVALID_SOCKET;
	WSADATA wsaData;

	//Init Socket
	if (WSAStartup(0x202, &wsaData) != 0)
	{
		cout << "\nLoi khoi tao socket\n";
		WSACleanup();
		return;
	}
	listenaddr.sin_family = AF_INET;
	listenaddr.sin_addr.s_addr = INADDR_ANY;
	listenaddr.sin_port = htons(8888);

	// Init Socket
	listensock = socket(AF_INET, SOCK_STREAM, 0);

	// check valid
	if (listensock == INVALID_SOCKET)
	{
		cout << "\nLoi : Khoi tao socket.";
		WSACleanup();
		return;
	}
	cout << ">> Da tao SOCKET.\n";
	//Bind Socket voi port 8888
	if (bind(listensock, (sockaddr*)& listenaddr, sizeof(listenaddr)) != 0)
	{
		cout << "\n Loi : bind socket.";
		WSACleanup();
		return;
	}
	//Bắt đầu lắng nghe các truy cập

	if (listen(listensock, SOMAXCONN) != 0)
	{
		cout << "\n Loi : listen.";
		WSACleanup();
		return;
	}
	cout << ">> Cho truy cap tu Client.\n";
	// bat dau lay client dua vao cac thread.
	while (true)
	{

		SOCKET ClientSocket = INVALID_SOCKET;
		ClientSocket = accept(listensock, nullptr, nullptr);
		if (ClientSocket == INVALID_SOCKET)
		{
			cout << "Loi Accept : " << WSAGetLastError();
			closesocket(ClientSocket);
			continue;
		}
		// Thread
		subThread = AfxBeginThread(ProxyServer, (LPVOID)ClientSocket);
	}
}

UINT ProxyServer(void* Iparam)
{
	mainThread = subThread;

	sock Client = (sock)Iparam;
	// nhan request da duoc gui di tu browser.
	char buffer[bufsize] = { 0 };
	int recvsize = recv(Client, buffer, bufsize, 0);


	std::size_t h1;
	string URLTOGET;
	if (recvsize > 0) {
		// lấy phần link thuộc method GET, và chuyển các ký tự đặc biệt sang _
		// để có thể lưu vào trong máy.
		URLTOGET = getCacheLink(buffer);
		URLTOGET = convertFileName(URLTOGET);
	}
	else
	{
		// nếu không còn request thì đóng socket
		Sleep(2000);
		closesocket(Client);
		return 0;
	}


	char domain[100] = { 0 };
	int port = 80;
	bool cangetdomain = false;
	cangetdomain = GetDomainName(buffer, domain, port);
	if (!isGETorPOSTmethod(buffer) || !cangetdomain || isHTTPs(buffer))
	{
		closesocket(Client);
		return 0;
	}

	cout << "===== REQUEST HEADER ====" << endl;
	cout << buffer << endl;

	cout << "===== CONNECTION INFO ====" << endl;
	cout << ">> HOST : " << domain << endl;

	// check blacklist
	// sử dụng map đã được khởi tạo để check blacklist cho domain.
	string tempdomain = domain;
	if (tempdomain.find("www.") == string::npos) tempdomain = "www." + tempdomain;
	if (domainblacklist[tempdomain]) {
		cout << "Blacklisted Site!" << endl;
		std::ifstream inFile;
		inFile.open("403.html"); //open the input file
		if (inFile.is_open())
		{
			std::stringstream strStream;
			strStream << inFile.rdbuf(); //read the file
			std::string str = strStream.str(); //str holds the content of the file
			int send403 = send(Client, str.c_str(), str.length(), 0);
		}
		inFile.close();
		closesocket(Client);
		return 0;
	}
	tempdomain.clear();

	// mở thread mới để nhận các yêu cầu khác từ client này.
	bool HTTP10 = isHTTP10(buffer);
	if (!HTTP10 && Client != INVALID_SOCKET)
	{
		// vì http1.0 không hỗ trợ 1 kết nối cho nhiều lần truyền dữ liệu nên k thể bật thread mới.
		// Thread
		WaitForSingleObject(mainThread, 1000);
		subThread = AfxBeginThread(ProxyServer, Iparam);
	}


	// kiêm tra xem đã tồn tại cache hay chưa, và xác định load từ cache hay webserver
	// nếu chưa hết hạn TTL thì vẫn sẽ load từ file cache 
	// load dữ liệu k cần connect vào webserver
	fstream cachefiletest(".\\cache\\" + URLTOGET, ios::binary | ios::in);
	if (cachefiletest.is_open())
	{
		int filesize = getFileSize(".\\cache\\" + URLTOGET);
		// đọc time lưu từ file để kiểm tra xem đã expire hay chưa
		fstream cacheTime(".\\time\\" + URLTOGET, ios::in);
		std::time_t cache_t;
		cacheTime >> cache_t;
		cacheTime.close();
		auto curTime = std::chrono::system_clock::now();
		std::time_t end_time = std::chrono::system_clock::to_time_t(curTime);

		int sec = end_time - cache_t;
		cout << ">> CACHE HAS LIVE FOR " << sec << " SECONDS" << endl;
		if (sec < cacheTTL)
		{
			cout << ">> CACHE FILE EXIST!" << endl;
			// doc from cache
			int cnt;
			cnt = filesize;
			while (cnt > 0)
			{
				int c = min(bufsize, cnt);
				cachefiletest.read(buffer, c);
				int b = send(Client, buffer, c, 0);
				cnt -= c;
			}
			cachefiletest.close();
			if (HTTP10) {
				closesocket(Client);
			}
			return 0;
		}
		else
		{
			cout << ">> CACHE FILE EXPIRED. " << endl;
		}
	}


	//Tao socket connect toi web server HTTP la port 80

	sock RemoteSocket = INVALID_SOCKET;
	//Tao cau truc dia chi de connect socket toi web server
	struct addrinfo hints, * infoptr; // So no need to use memset global variables
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_family = AF_INET; // AF_INET means IPv4 only addresses
	int iResult = getaddrinfo(domain, std::to_string(port).c_str(), &hints, &infoptr);
	if (iResult != 0)
	{
		printf(">> Getaddrinfo failed with error: %d\n", iResult);
		closesocket(Client);
		closesocket(RemoteSocket);
		return 0;
	}

	int connectTime = 0;
	for (auto ptr = infoptr; ptr != NULL, connectTime < MAXTIMETORETRY; ptr = ptr->ai_next)
	{
		if (connectTime == MAXTIMETORETRY)
		{
			closesocket(Client);
			closesocket(RemoteSocket);
			return 0;
		}
		//Create socket
		RemoteSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (RemoteSocket == INVALID_SOCKET)
		{
			cout << "Loi : Tao Remote Socket :  " << WSAGetLastError() << endl;
			closesocket(RemoteSocket);
			connectTime++;
			continue;
		}

		// Connect to server.
		iResult = connect(RemoteSocket, infoptr->ai_addr, (int)infoptr->ai_addrlen);
		if (iResult == SOCKET_ERROR)
		{
			closesocket(RemoteSocket);
			RemoteSocket = INVALID_SOCKET;
			connectTime++;
			continue;
		}
		break;
	}
	freeaddrinfo(infoptr);
	cout << "Da connect toi webserver.\n\n";

	// gui request tu client toi webserver.
	recvsize = send(RemoteSocket, buffer, recvsize, 0);
	if (recvsize == SOCKET_ERROR)
	{
		cout << ">> ERROR : Cannot send request header to WebServer." << endl;
		closesocket(RemoteSocket);
		return 0;
	}
	
	// in ra tên của file được cache.
	cout << "====== CACHE FILE INFO ======" << endl;
	cout << ">> URL : " << URLTOGET << endl;

	
	// nhận về phần header của response
	int endhead = 0;
	string header;
	int bytes;
	char temp;
	while (endhead < 4)
	{
		bytes = recv(RemoteSocket, &temp, 1, 0);
		header.push_back(temp);
		if (temp == '\r' || temp == '\n')
			endhead++;
		else endhead = 0;
	}

	cout << "===== RESPONSES HEADER ====" << endl;
	cout << header << endl;
	send(Client, header.c_str(), header.length(), 0);

	//kiểm tra mã 304 xem browser đã cache hay chưa
	int pos = header.find("304 Not Modified");
	if (pos != string::npos)
	{
		cout << ">> CODE 304 FOUND : Proxy Server Exit!!." << endl;
		closesocket(Client);
		closesocket(RemoteSocket);
		return 0;
	}

	// kiểm tra xem cache control có tag public hay không
	// tag public cho phép shared cache : như của proxy server.
	bool canCDNCache = checkCacheControl(header);
	cout << ">> CACHE NOT EXIST!" << endl;
	int bytes_rev = 0;
	fstream out;
	if (canCDNCache == true)
	{
		// lưu time mà proxy server cache file này.
		// sẽ update sau 1h
		out.open(".\\cache\\" + URLTOGET, ios::binary | ios::out);
		out << header;
		auto curTime = std::chrono::system_clock::now();
		std::time_t end_time = std::chrono::system_clock::to_time_t(curTime);
		ofstream outTime(".\\time\\" + URLTOGET,ios::out);
		outTime << end_time;
		outTime.close();
	}
	else
	{
		out.close();
	}
	
	while ((bytes_rev = recv(RemoteSocket, buffer, bufsize, 0)) > 0)
	{
		if (bytes_rev > 0) {
			cout << "Bytes received: " << bytes_rev << endl;
			//backup body to proxy cache
			if (out.is_open())
			{
				out.write(buffer, bytes_rev);
				out.flush();
			}
			// Echo the buffer back to the sender
			send(Client, buffer, bytes_rev, 0);
		}
	}

	if (out.is_open())
	{
		out.close();
	}

	closesocket(RemoteSocket);
	closesocket(Client);
	return 1;
}


bool isGETorPOSTmethod(char* req)
{
	string s(req);
	if (s.find("GET") != std::string::npos || s.find("POST") != std::string::npos)
	{
		return true;
	}
	return false;
}

bool GetDomainName(char* request, char* dname, int& port)
{
	int n = strlen(request), pos = 0, i = 0;
	n -= 6;
	for (; i < n; i++)
	{
		if (request[i] == 'H' && request[i + 1] == 'o' && request[i + 2] == 's' && request[i + 3] == 't'
			&& request[i + 4] == ':')
		{
			pos = i + 6;
			break;
		}
	}
	if (i == n)
		return false;
	i = 0;
	while (pos < n && request[pos] != '\r' && request[pos] != ':')
	{
		dname[i] = request[pos];
		pos++;
		i++;
	}
	dname[i] = '\0';

	string hostname_t(request);
	//tra ve cac port phu cua http neu nam trong url
	if (hostname_t.find(":8080") != string::npos)
	{
		port = 8080;
	}
	else if (hostname_t.find(":8008") != string::npos)
	{
		port = 8008;
	}
	else
	{
		port = 80;
	}
	return true;
}


bool isHTTPs(char* req)
{
	string dom(req);
	if (dom.find(":443") != std::string::npos)
	{
		return true;
	}
	return false;
}

bool isHTTP10(char* request)
{
	string req(request);
	if (req.find("HTTP/1.0") != std::string::npos)
	{
		return true;
	}
	return false;
}

int getLengthOfFile(string head)
{
	int pos = head.find("Content-Length: ");
	int sum = 0;
	if (pos == string::npos || pos == -1)
		return -1;
	else {
		pos += 16;
		while (head[pos] != '\r' && head[pos] != '\n')
		{
			sum += head[pos] - '0';
			sum *= 10;
			pos++;
		}
		sum /= 10;
		return sum;
	}
}

int getFileSize(const std::string& fileName)
{
	ifstream file(fileName.c_str(), ifstream::in | ifstream::binary);

	if (!file.is_open())
	{
		return -1;
	}

	file.seekg(0, ios::end);
	int fileSize = file.tellg();
	file.close();

	return fileSize;
}


string getCacheLink(char* request)
{
	string req(request);
	int pos = req.find("GET");
	if (pos == string::npos)
	{
		return "no_valid";
	}
	pos += 4;
	int end = pos;
	if (end > 200)
	{
		return "no_valid";
	}
	for (end = pos + 1; pos < req.length(); end++)
	{
		if (req[end + 1] == ' ')
		{
			break;
		}
	}
	string res = req.substr(pos, end - pos + 1);
	return res;
}


string convertFileName(string linktemp)
{
	string link = linktemp;
	for (int i = 0; i < link.length(); i++)
	{
		if (link[i] == 92 || link[i] == 47 || link[i] == 58 || link[i] == 42
			|| link[i] == 63 || link[i] == 34 || link[i] == 60 || link[i] == 62 || link[i] == 124)
		{
			link[i] = '_';
		}
	}
	return link;
}


bool checkCacheControl(string res)
{
	if (res.find("Cache-Control:") != string::npos)
	{
		if (res.find("public") != string::npos)
		{
			return true;
		}
		return false;
	}
	return true;
}
