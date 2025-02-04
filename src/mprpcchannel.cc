#include "mprpcchannel.h"
#include "rpcheader.pb.h"
#include "mprpcapplication.h"
#include "zookeeperutil.h"
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <unistd.h>

// 负责Caller方的序列化和发送工作,所有通过stub代理对象调用的rpc方法，都会走到这里
void mpRpcChannel::CallMethod(const google::protobuf::MethodDescriptor* method,
                          google::protobuf::RpcController* controller, const google::protobuf::Message* request,
                          google::protobuf::Message* response, google::protobuf::Closure* done)
{
    const google::protobuf::ServiceDescriptor* service =  method->service();
    std::string service_name = service->name();
    std::string method_name = method->name();

    uint32_t args_size;
    std::string args_str;
    // 将调用方法的参数序列化
    if(request->SerializeToString(&args_str)){
        // 序列化成功
        args_size = args_str.size();
    }else{
        // 序列化失败
        controller->SetFailed("Serializa args_str failed");
        return;
    }
    // 定义rpc请求header
    mprpc::RpcHeader rpc_header;
    rpc_header.set_method_name(method_name);
    rpc_header.set_service_name(service_name);
    rpc_header.set_args_size(args_size);

    std::string rpc_header_str;
    // 数据头大小,以4字节填入send_rpc_str
    uint32_t header_size;
    if(rpc_header.SerializePartialToString(&rpc_header_str)){
        // 序列化成功
        header_size = rpc_header_str.size();
    }else{
        // 序列化失败
        controller->SetFailed("Serialize rpc_header_str failed");
        return;
    }

    /*
        header_size + header_str + args_str
    */ 
    std::string send_rpc_str;
    send_rpc_str.insert(0,std::string((char*)&header_size,4));
    send_rpc_str += rpc_header_str;
    send_rpc_str += args_str;

    std::cout << "================================" << std::endl;
    std::cout << "header size :" << header_size << std::endl;
    std::cout << "header str :" << rpc_header_str << std::endl;
    std::cout << "args str :" <<args_str << std::endl;
    std::cout << "================================" << std::endl;    

    int clientfd = socket(AF_INET,SOCK_STREAM,0);
    if(-1 == clientfd){
        char errtxt[512];
        sprintf(errtxt,"create socket error ! errno : %d",errno);
        controller->SetFailed(errtxt);
        return;
    }
    
    // 读取配置文件中服务端得IP地址和端口号
    //std::string ip = mprpcapplication::GetInstance().GetConfig().Load("rpcservicesIP");
    //uint16_t port = atoi(mprpcapplication::GetInstance().GetConfig().Load("rpcservicesPort").c_str());
    
    // 读取zk服务端中注册好的方法服务，也就是获取方法所属的IP和PORT
    zkClient zkcli;
    zkcli.Start();
    std::string method_path = "/" + service_name + "/" + method_name;
    std::string host_data = zkcli.GetData(method_path.c_str());
    if(host_data == ""){
        controller->SetFailed("method path data is null");
        return;
    }
    size_t index = host_data.find(":");
    if(-1 == index){
        controller->SetFailed("method path : " + method_name +" is invalid");
        return;
    }
    std::string ip = host_data.substr(0,index);
    uint16_t port = atoi(host_data.substr(index + 1,host_data.size() - index).c_str());

    // 采用tcp通信，发送请求报文 
    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    server.sin_addr.s_addr = inet_addr(ip.c_str());

    if(-1 == connect(clientfd,(sockaddr*)&server,sizeof(server))){
        close(clientfd);
        char errtxt[512];
        sprintf(errtxt,"connect server error ! errno : %d",errno);
        controller->SetFailed(errtxt);
        return;
    }

    if(-1 == send(clientfd,send_rpc_str.c_str(),send_rpc_str.size(),0)){
        close(clientfd);
        char errtxt[512];
        sprintf(errtxt,"send msg to server error ! errno : %d",errno);
        controller->SetFailed(errtxt);
        return;
    }

    char recv_buf[1024] = {0};
    int recv_size = 0;
    if(-1 == (recv_size = recv(clientfd,recv_buf,1024,0))){
        close(clientfd);
        char errtxt[512];
        sprintf(errtxt,"recv msg from server error ! errno : %d",errno);
        controller->SetFailed(errtxt);
        return;
    }

    //std::string response_str(recv_buf,0,recv_size);
    //if(!response->ParseFromString(response_str)){
    if(!response->ParseFromArray(recv_buf,recv_size)){
        close(clientfd);
         char errtxt[512];
        sprintf(errtxt,"response str parse error ! errno : %d",errno);
        controller->SetFailed(errtxt);
        return;
    }

    std::string debug;
    response->SerializeToString(&debug);
    
    
    close(clientfd);
}