#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <netinet/in.h>
#include <signal.h>
#include <dirent.h> 

#define N_THREADS 4
#define PORT 8080
#define BUFFER_SIZE 1024

typedef struct file {
	char* name;
	struct file* next;
} file;

char* internal_error = "HTTP/1.1 500 Internal Server Error\n"
      "Content-type: text/html\n"
      "\n"
      "<html>\n"
      " <body>\n"
      "  <h1>Internal server error</h1>\n"
      " </body>\n"
      "</html>\n";
	
char* not_found_response_template = 
      "HTTP/1.1 404 Not Found\n"
      "Content-type: text/html\n"
      "\n"
      "<html>\n"
      " <body>\n"
      "  <h1>Not Found</h1>\n"
      "  <p>The requested URL was not found on this server.</p>\n"
      " </body>\n"
      "</html>\n";

char* no_connection_available = "HTTP/1.1 429 Too Many Requests\n"
      "Content-type: text/html\n"
      "\n"
      "<html>\n"
      " <body>\n"
      "  <h1>Connection not available now. Please, try again later</h1>\n"
      " </body>\n"
      "</html>\n";
char* no_file_reply = "Requested page does not exist";
char* insecure_file = "Insecure file was queried";
int writeDataToClient(int, const void*, int);
int getFileSize(FILE*);
char* getFileContent(FILE*, int);
int writeStrToClient(int, char*);
char* extractFileName(char*);
void* serverReply(void*);
int getFreeThread();
void releaseThread(int);
void myHandler(int);
void waitForAllThreadsFree();
int fileIsAvailable(const char*);
void addNewFile(char*);
void printFileList();
int isDirectory(const char*);
void addAllFiles(const char*);
void initializeMutexes();
void initializeBuffers();
void initializeThreadsFree();
void initializeThreads();
void releaseSocketMutexes();

FILE* logfile;
file* head = NULL;
int server_fd, new_socket, val_read;
int* sckt;
int opt = 1;
int terminating = 0;
char** buffer;
struct sockaddr_in address;
int addrlen = sizeof(address);
struct stat st;
pthread_mutex_t* sckt_mutex;
pthread_mutex_t thread_free_mutex;
int* thread_free;
pthread_t* threads;

int main(int argc, const char** argv) {
	signal(SIGINT, myHandler);
	logfile = fopen("logfile.log", "a+");
    int i = 0;
	
	// Initializing buffers
	initializeBuffers();
	
	// Initializing mutexes
	initializeMutexes();
	
	// Initializing each thread as not busy
	initializeThreadsFree();
	
	// Creating a list of all files available in this directory and its subdirectories
	addAllFiles(".");

    // Creating socket file descriptor
    if((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }
    
    // Attaching socket to the port 8080
    if(setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }
	
	// Setting server address options
    address.sin_family = AF_INET; 
    address.sin_addr.s_addr = inet_addr("127.0.0.1");      
    address.sin_port = htons(PORT);

    // Attaching the socket address
    if(bind(server_fd, (struct sockaddr*)&address, addrlen) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    
	// Opening a passive socket
    if(listen(server_fd, N_THREADS) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
	
	// Creating a pool of threads
	initializeThreads();

    while(1) {
		// Accepting a new connection
        if((new_socket = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0) {
            perror("accept");
            exit(EXIT_FAILURE);
        }
				
		/* Looking for an idle thread
		If it exists: attaching connection service to it 
		Else: closing the connection */
	
		pthread_mutex_lock(&thread_free_mutex); 
		
		if((i = getFreeThread()) != -1) {
			sckt[i] = new_socket;
			thread_free[i] = 0;
			printf("Thread %d starts\n", i);
			pthread_mutex_unlock(&sckt_mutex[i]);
			pthread_mutex_unlock(&thread_free_mutex);	
		} else {
			fprintf(logfile, "%s\n", "All threads are busy\n");
			writeStrToClient(new_socket, no_connection_available);
			close(new_socket);	
		}
		
		pthread_mutex_unlock(&thread_free_mutex);
	}
    return 0;
}

void initializeBuffers() {
	int i;
	buffer = (char**)malloc(sizeof(char*)*N_THREADS);
	
	for(i = 0; i < N_THREADS; i++) {
		buffer[i] = (char*)malloc(sizeof(char)*BUFFER_SIZE);
	}
}

void initializeThreadsFree() {
	int i;
	thread_free = (int*)malloc(sizeof(int)*N_THREADS);
	
	for(i = 0; i < N_THREADS; i++) {
		thread_free[i] = 1;	
	}
}

void initializeMutexes() {
	int i;
	sckt_mutex = (pthread_mutex_t*)malloc(sizeof(pthread_mutex_t)*N_THREADS);
	
	for(i = 0; i < N_THREADS; i++) {
		pthread_mutex_init(&sckt_mutex[i], NULL);
		pthread_mutex_lock(&sckt_mutex[i]);
	}
	pthread_mutex_init(&thread_free_mutex, NULL);
}

int getFreeThread() {
	int i;
	for(i = 0; i < N_THREADS; i++) {
		if(thread_free[i] == 1)
			return i;
	}
	return -1;
}

void initializeThreads() {
	int i;
	sckt = (int*)malloc(sizeof(int)*N_THREADS);
	threads = (pthread_t*)malloc(sizeof(pthread_t)*N_THREADS);
	
	for(i = 0; i < N_THREADS; i++) {
		int* thread_id = (int*)malloc(sizeof(int));
		*thread_id = i;
		pthread_create(&threads[i], NULL, serverReply, (void*)thread_id);	
	}
}

void releaseThread(int thread_id) {
	thread_free[thread_id] = 1;
	close(sckt[thread_id]);
}

int writeStrToClient(int sckt, char* string) {
    return writeDataToClient(sckt, string, strlen(string));
}

char* extractFileName(char* string) {
    const char* p1 = strstr(string, "GET /") + 5;
    const char* p2 = strstr(p1, " HTTP");
    size_t len = p2 - p1;
    char* res = (char*)malloc(len+1);
    strncpy(res, p1, len);
    res[len] = '\0';
    return res;
}

char* getFileContent(FILE* fp, int fsize) {
    char* content = (char*)malloc(fsize);
    
    if(!content) {
        perror("malloc");
        return NULL;
    }

    if(fread(content, fsize, 1, fp) != 1) {
        perror("fread");
        return NULL;
    }
    
    return content;
}

int writeDataToClient(int sckt, const void* data, int datalen) {
    const char* pdata = (const char*)data;

    while(datalen > 0) {
        int numSent = send(sckt, pdata, datalen, 0);
        
        if(numSent <= 0) {
            perror("send");
            return 0;
        }

        pdata += numSent;
        datalen -= numSent;
    }

    return 1;
}

int getFileSize(FILE* fp) {
    int start = ftell(fp);
    fseek(fp, 0L, SEEK_END);
    int size = ftell(fp);
    fseek(fp, start, SEEK_SET);

    return size;
}

int fileIsAvailable(const char* filename) {
	file* ptr = head;	
	
	while(ptr != NULL) {
		if(!strcmp(ptr->name, filename))
			return 1;
		ptr = ptr->next;	
	}	
	return 0;
}

void addAllFiles(const char* path) {
	DIR *d;
	struct dirent *dir;
	d = opendir(path);
	char filename[100];
	if (d) {
		while ((dir = readdir(d)) != NULL) {
			sprintf(filename, "%s/%s", path, dir->d_name);
			if(!isDirectory(dir->d_name)) {
	  			addNewFile(filename);				
			} else if (strcmp(dir->d_name, ".") > 0 && strcmp(dir->d_name, "..") > 0) {
				addAllFiles(filename);	
			}
		}
		closedir(d);
	}
}

int isDirectory(const char *path) {
	struct stat statbuf;
	if (stat(path, &statbuf) != 0)
	   return 0;
	return S_ISDIR(statbuf.st_mode);
}

void addNewFile(char* name) {
	file* f = (file*)malloc(sizeof(file));
	f->name = strdup(name + 2);
	f->next = head;
	head = f;
}

void* serverReply(void* arg) {
	int thread_id = *(int*)arg;
	
	while(1) {
		pthread_mutex_lock(&sckt_mutex[thread_id]);
		if(terminating) {
			printf("Thread %d is terminating\n", thread_id);
			break;	
		}
		val_read = read(sckt[thread_id], buffer[thread_id], BUFFER_SIZE);
		char* response = strdup(buffer[thread_id]);
		//fprintf(logfile, "%s\n", buffer);

		FILE* fp;

		/* Getting the file name to be sent
		Checking if it is in my list
		If it's not: closing the connection 
		Else: opening it*/
		
		char* filename = extractFileName(response);
		if(!fileIsAvailable(filename)) {
			writeStrToClient(sckt[thread_id], not_found_response_template);
			perror("Not available file");
			fprintf(logfile, "Insecure file inquiry\n");
			releaseThread(thread_id);
			continue;
		}
		
		fprintf(logfile, "%s is queried\n", filename);       

		if((fp = fopen(filename, "rb")) == NULL) {
			writeStrToClient(sckt[thread_id], not_found_response_template);
			perror("fopen");
			fprintf(logfile, "%s\n", no_file_reply);
			releaseThread(thread_id);
			continue;
		}

		// Getting the file size and content
		int fsize = getFileSize(fp);
		char* msg = getFileContent(fp, fsize);
		fclose(fp);

		// Establishing an HTTP connection
		if(!writeStrToClient(sckt[thread_id], "HTTP/1.1 200 OK\r\n")) {
			perror("HTTP");
			fprintf(logfile, "HTTP error\n");
			writeStrToClient(sckt[thread_id], internal_error);
			releaseThread(thread_id);
			continue;
		}

		char clen[40];
		sprintf(clen, "Content-length: %d\r\n", fsize);

		if(!writeStrToClient(sckt[thread_id], clen)) {
			perror("write content length");
			fprintf(logfile, "Error in sending content length\n");
			writeStrToClient(sckt[thread_id], internal_error);
			releaseThread(thread_id);
			continue;
		}

		if(!writeStrToClient(sckt[thread_id], "Content-Type: text/html\r\n")) {
			perror("Content Type");
			fprintf(logfile, "Error in sending content type\n");
			writeStrToClient(sckt[thread_id], internal_error);
			releaseThread(thread_id);
			continue;
		}

		if(writeStrToClient(sckt[thread_id], "Connection: close\r\n\r\n") < 0) {
			perror("close connection");
			writeStrToClient(sckt[thread_id], internal_error);
			releaseThread(thread_id);
			continue;
		}

		// Sending the queried data to the client
		if(!writeDataToClient(sckt[thread_id], msg, strlen(msg))) {
			perror("writeDataToClient");
			fprintf(logfile, "Error in sending data\n");
			writeStrToClient(sckt[thread_id], internal_error);
			releaseThread(thread_id);
			continue;
		}
		
		printf("Thread %d finished\n", thread_id);
		fprintf(logfile, "%s sent to the client\n", filename);
		
		//sleep(1);
		
		// Closing the connection
		pthread_mutex_lock(&thread_free_mutex);
		releaseThread(thread_id);
		pthread_mutex_unlock(&thread_free_mutex);
	}
	return NULL;
}

void waitForAllThreadsFree() {
	int i;
	for(i = 0; i < N_THREADS; i++) {
		pthread_join(threads[i], NULL);
	}
}

void releaseSocketMutexes() {
	int i;
	
	for(i = 0; i < N_THREADS; i++) {
		pthread_mutex_unlock(&sckt_mutex[i]);	
	}
}

void myHandler(int sig) {
	terminating = 1;
	releaseSocketMutexes();
	waitForAllThreadsFree();
	close(new_socket);
	while(shutdown(server_fd, 2) < 0);
	fprintf(logfile, "Successfully terminating\n");
	fclose(logfile);
	exit(0);	
}







