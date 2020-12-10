#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <semaphore.h>
 

/*  debug log
    1. only one mcu and android can access to server --> fixed !
    2. approx real time to get tcp connect status with android --> fixed!
    3. for android, it's no need to set heartbeat, in android api it judge the tcp status in real time,
    but for server, it should setup heartbeat to judge wether clients are still alive !
    4. when the app is forced to exit, encounter bad file descriptor and hang over -- because and_alive is not reset --> fixed !
    5. fix the listview of app exit abnormal when update data, keep in mind do not update screen in other thread except UIthread --> fixed!
    6. move all "sem_post" to heartbeat, it will accelerate linking error detecting!! --> fixed
*/
 
#define SBUF_SIZE 16
#define RSBUF_SIZE 64

struct Clients{
    int mcu_sockfd;
    int and_sockfd;
};

struct Clients myClients = {-1, -1};

int server_sockfd;//服务器端套接字
int client_sockfd;//客户端套接字
struct sockaddr_in my_addr;   //服务器网络地址结构体
struct sockaddr_in remote_addr; //客户端网络地址结构体

pthread_mutex_t mcu_lock;
pthread_mutex_t and_lock;
sem_t signal;

pthread_t mcuRecThread;
pthread_t andRecThread;
pthread_t mcuHeartbeatThread;
pthread_t andHeartbeatThread;

pthread_attr_t mcuRTHattr;
pthread_attr_t andRTHattr;
pthread_attr_t mcuHTHattr;
pthread_attr_t andTHTattr;


int heartbeat = 0;
int mcu_alive = 0;
int and_alive = 2;

int readPtr = 0;
int rec_flag = 0;
char readBuff[RSBUF_SIZE] = {0};

void setThreadAttr(pthread_attr_t *attr, int pro);
void createThread(pthread_t *pth, pthread_attr_t *attr ,void*(*p)());
void recCallback();
void *rec_from_mcu();
void *rec_from_and();
void *mcu_heartbeats();
void *and_heartbeats();
int analyseData(char* rbuf, int len);
void analyseDataForMcu(char* rbuf, int len);



void setThreadAttr(pthread_attr_t *attr, int pro){
    pthread_attr_init(attr);
    struct sched_param schp = {pro};
    pthread_attr_setschedpolicy(attr, SCHED_RR);
    pthread_attr_setschedparam(attr, &schp);
}

void createThread(pthread_t *pth, pthread_attr_t *attr ,void*(*p)()){

    int ret;
    ret = pthread_create(pth, attr, p, NULL);

    if(ret == -1)   perror("thread creat error");
}

void recCallback(){
    char res[readPtr+1];
    for(int i=0; i<readPtr; i++)    res[i] = readBuff[i];
    res[readPtr] = '\0';
    //printf("client: %s\n", res);
    if((strcmp(res, "mcu") == 0) && (myClients.mcu_sockfd == -1)){
        //printf("mcu is connected\n");
        mcu_alive = 1;
        heartbeat = 0;
        myClients.mcu_sockfd = client_sockfd;
        setThreadAttr(&mcuRTHattr, 50);
        setThreadAttr(&mcuHTHattr, 60);
        createThread(&mcuRecThread, &mcuRTHattr, rec_from_mcu);
        createThread(&mcuHeartbeatThread, &mcuHTHattr, mcu_heartbeats);
        if(and_alive)   send(myClients.and_sockfd, "mcu\n", 4, 0);
    }else if((strcmp(res, "and")==0) && (myClients.and_sockfd == -1)){
        //printf("and is connected\n");
        and_alive = 2;
        myClients.and_sockfd = client_sockfd;
        setThreadAttr(&andRTHattr, 50);
        setThreadAttr(&andTHTattr, 60);
        createThread(&andRecThread, &andRTHattr, rec_from_and);
        createThread(&andRecThread, &andTHTattr, and_heartbeats);
        if(mcu_alive)   send(myClients.and_sockfd, "mcu\n", 4, 0);
    }else{
        //printf("wrong client!!\n");
        close(client_sockfd);
        sem_post(&signal);
    }
}

int analyseData(char* rbuf, int len){
    for(int i=0; i<len; i++){
        if(rbuf[i] == '@'){
            readPtr = 0;
            rec_flag = 1;
            continue;
        }else if(rbuf[i] == '#'){
            if((readPtr > 0) && (rec_flag ==1)){
                recCallback();
                rec_flag = 0;
                return 0;
            }
            rec_flag = 0;
            continue;
        }
        
        if(rec_flag){
            readBuff[readPtr++] = rbuf[i];
            if(readPtr >= RSBUF_SIZE-2){
                rec_flag = 0;
                readPtr = 0;
            }
        }
    }
    return 1;
}

void waitForClients(){

    char rbuf[RSBUF_SIZE];
    int not_get_id = 1;

    socklen_t sin_size;
    if(listen(server_sockfd,5)<0){
        perror("listen error");
        return;
    }

    while(1){
        sem_wait(&signal);
        /*监听连接请求--监听队列长度为5*/
        sin_size=sizeof(struct sockaddr_in);
        //printf("wait for clients...\n");
        if((client_sockfd=accept(server_sockfd,
            (struct sockaddr *)&remote_addr,&sin_size))<0){
                perror("accept error");
                sem_post(&signal);
                continue;
            }
        //printf("check client's id\n");
        not_get_id = 1;
        while(not_get_id){
            int len = recv(client_sockfd,rbuf,RSBUF_SIZE,0);
            if(len > 0){
                not_get_id = analyseData(rbuf, len);
            }else if((len <=0) && (errno == EINTR)){
                perror("network error1");
            }else{
                perror("network error2");
                close(client_sockfd);
                sem_post(&signal);
                not_get_id = 0;
            }   
        }
    }
}

void *and_heartbeats(){
    char tcp[] = "tcp\n";
    int len;
    // 5s tell the tcp is still alive
    while(and_alive){
        // spare code
        // len = send(myClients.and_sockfd, tcp, sizeof(tcp)-1, 0); 
        // if(len <= 0)    break;
        sleep(4);
        pthread_mutex_lock(&and_lock);
        and_alive--;
        pthread_mutex_unlock(&and_lock);
    }
    //printf("and tcp breaking\n");
    close(myClients.and_sockfd);
    sem_post(&signal);
    myClients.and_sockfd = -1;
}

void* mcu_heartbeats()
{
    while(mcu_alive){
        pthread_mutex_lock(&mcu_lock);
        heartbeat ++;
        if(heartbeat > 4){
            perror("tcp break");
            mcu_alive = 0;
        }
        pthread_mutex_unlock(&mcu_lock);
        sleep(1);
    }
    //printf("mcu tcp breaking\n");
    close(myClients.mcu_sockfd);
    sem_post(&signal);
    myClients.mcu_sockfd = -1;
    mcu_alive = 0;
    heartbeat = 0;
    if(and_alive)   send(myClients.and_sockfd, "dmcu\n", 5, 0);
}

void* rec_from_mcu()
{
    int len = 0;
    char rbuf[RSBUF_SIZE] = {0};  //数据传送的缓冲区
    char beat[5] = {0};
    /*接收客户端的数据并将其发送给客户端--recv返回接收到的字节数，send返回发送的字节数*/

    while(mcu_alive){
        len = recv(myClients.mcu_sockfd,rbuf,RSBUF_SIZE,0);
        if(len > 0){
            for(int i=0; i<4; i++)  beat[i] = rbuf[i];      
            if(strcmp(beat, "beat") == 0){
                pthread_mutex_lock(&mcu_lock);
                heartbeat = 0;
                pthread_mutex_unlock(&mcu_lock);
                continue;
            }
            // //printf rbuf to be sent to and
            rbuf[len]='\0';
            //printf("%s\n",rbuf);

            rbuf[len] = '\n';
            if(send(myClients.and_sockfd, rbuf, len+1, 0) <= 0){
                //printf("failed to send to and, check for and's tcp\n");
            }
        }else if((len <=0) && (errno == EINTR)){

        }else{
            perror("network error");
            break;
        }
    }
}

// float temp = 25.7;  float tum = 75.8;
void analyseDataForMcu(char* rbuf, int len){
    for(int i=0; i<len; i++){
        if(rbuf[i] == '@'){
            readPtr = 0;
            rec_flag = 1;
            continue;
        }else if(rbuf[i] == '#'){
            if((readPtr > 0) && (rec_flag ==1)){
                if((readBuff[0] == 'f') && (readBuff[1] == 'd')){
                    char cmd[] = "fd";
                    //printf("and: fd\n");
                    send(myClients.mcu_sockfd, cmd, sizeof(cmd), 0);
                }
                if((readBuff[0] == 'w') && (readBuff[1] == 's')){
                    char cmd[] = "ws";
                    //printf("and: ws\n");
                    // temp += 0.2;    tum += 0.2;
                    // char echo[16] = {0};
                    // sprintf(echo, "w%.1f\ns%.1f\n", temp, tum);
                    //for test
                    // send(myClients.and_sockfd, echo, strlen(echo), 0);
                    send(myClients.mcu_sockfd, cmd, sizeof(cmd), 0);
                }
                if((readBuff[0] == 't') && (readBuff[1] == 'c') && (readBuff[2] == 'p')){
                    pthread_mutex_lock(&and_lock);
                    and_alive = 2;
                    pthread_mutex_unlock(&and_lock);
                }
            }
            rec_flag = 0;
            continue;
        }
        
        if(rec_flag){
            readBuff[readPtr++] = rbuf[i];
            if(readPtr >= RSBUF_SIZE-2){
                rec_flag = 0;
                readPtr = 0;
            }
        }
    }
}

void *rec_from_and(){
    int len;
    char rbuf[RSBUF_SIZE] = {0};
    while(1){
        len = recv(myClients.and_sockfd,rbuf,RSBUF_SIZE,0);
        if(len > 0){
            analyseDataForMcu(rbuf, len);
        }else if((len <=0) && (errno == EINTR)){
            perror("rec network error1");
        }else{
            perror("rec network error2");
            break;
        }
    }

    close(myClients.and_sockfd);
    myClients.and_sockfd = -1;
}


void initServer()
{
    int sin_size;
    memset(&my_addr,0,sizeof(my_addr)); //数据初始化--清零
    my_addr.sin_family=AF_INET; //设置为IP通信
    my_addr.sin_addr.s_addr=INADDR_ANY;//服务器IP地址--允许连接到所有本地地址上
    my_addr.sin_port=htons(8193); //服务器端口号
    
    /*创建服务器端套接字--IPv4协议，面向连接通信，TCP协议*/
    if((server_sockfd=socket(PF_INET,SOCK_STREAM,0))<0)
    {  
        perror("socket error");
    }
    int on = 1;
    if(setsockopt(server_sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(int)) < 0){
    	perror("setsockopt error");
    }
 
    /*将套接字绑定到服务器的网络地址上*/
    if(bind(server_sockfd,(struct sockaddr *)&my_addr,sizeof(struct sockaddr))<0)
    {
        perror("bind error");
    }
    
    // set timeout
    // when exceed 1 min will break loop
    // struct timeval timeout = {60, 0};
    // if(setsockopt(server_sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout)) != 0){
    // 	perror("set send timeout failed");
    // }

    // if(setsockopt(server_sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) != 0){
    // 	perror("set receive timeout failed");
    // }

}



int main(int argc, char *argv[])
{

    int ret = pthread_mutex_init(&mcu_lock, NULL);
    if (ret != 0) {
        //printf("mutex init failed\n");
        return -1;
    }

    ret = pthread_mutex_init(&and_lock, NULL);
    if (ret != 0) {
        //printf("mutex init failed\n");
        return -1;
    }

    ret = sem_init(&signal, 2, 2);
    if (ret != 0) {
        //printf("sem init failed\n");
        return -1;
    }

    initServer();
    waitForClients();
    close(server_sockfd);
    return 0;
}
