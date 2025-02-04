#include "rpcprovider.h"
#include "mprpcapplication.h"
#include "logger.h"
#include "zookeeperutil.h"

using namespace std::placeholders;

// 这里是框架提供给外部使用的，可以发布rpc方法
void RpcProvider::NotifyService(::google::protobuf::Service* service)
{
    ServiceInfo service_info;

    // 获取服务对象的描述信息
    const google::protobuf::ServiceDescriptor *pserviceDesc =  service->GetDescriptor();
    // 从描述信息中拿到服务对象名字
    std::string service_name = pserviceDesc->name();
    // 从描述信息中拿到服务对象中方法的数量
    int method_count = pserviceDesc->method_count();

    LOG_INFO("Log in service name : %s",service_name);
    std::cout << "service name :" << service_name << std::endl;

    // 登记映射关系表
    for(int i = 0;i < method_count;++i)
    {
        const google::protobuf::MethodDescriptor *pmethodDesc = pserviceDesc->method(i);
        std::string method_name = pmethodDesc->name();
        
        LOG_INFO("Log in method name : %s",method_name);
        std::cout << "method name : " << method_name << std::endl;
        
        service_info.m_methodMap.insert({ method_name , pmethodDesc});
    }
    service_info.m_service = service;

    m_serviceMap.insert({service_name , service_info});
}
/* 
    为provider节点建立网络服务，注册事件回调，将Notify中注册好的服务添加到zk服务器。
    构建临时性节点，超时时间为30s，心跳消息发送间隔：1/3 * 超时时间
*/

void RpcProvider::Run()
{
    std::string ip = mprpcapplication::GetInstance().GetConfig().Load("rpcservicesIP");
    uint16_t port = std::atoi( mprpcapplication::GetInstance().GetConfig().Load("rpcservicesPort").c_str());
    muduo::net::InetAddress inetaddress(ip,port);
    
    // 组合TcpServer对象
    muduo::net::TcpServer tcpserver(&_eventloop,inetaddress,"rpcprovider");
    // 注册用户连接创建回调
    tcpserver.setConnectionCallback(std::bind(&RpcProvider::OnConnection,this,_1));
    // 注册用户用户读写事件回调
    tcpserver.setMessageCallback(std::bind(&RpcProvider::OnMessage,this,_1,_2,_3));

    tcpserver.setThreadNum(4);

    // 注册服务到zk服务段中
    zkClient zkCli;
    zkCli.Start();

    for(auto &sp : m_serviceMap){
        // service_name为永久性节点，method_name为临时性节点
        std::string service_path = "/" + sp.first;
        zkCli.Create(service_path.c_str(),nullptr,0);
        
        for(auto &mp : sp.second.m_methodMap){
            std::string method_str = service_path + "/" + mp.first;
            char method_path_data[128] = {0};
            sprintf(method_path_data,"%s:%d",ip.c_str(),port);
            zkCli.Create(method_str.c_str(),method_path_data,strlen(method_path_data),ZOO_EPHEMERAL);
        }
    }



    tcpserver.start();
    
    LOG_INFO("[RpcProvider service start] serviceIP : %s servicePort : %d",ip,port);
    std::cout << "[RpcProvider service start] " << "serviceIP :" << ip << "servicePort :" << port << std::endl;
    
    _eventloop.loop();

}

void RpcProvider::OnConnection(const muduo::net::TcpConnectionPtr& conn)
{
    if(!conn->connected())
    {
        conn->shutdown();
    }
}

void RpcProvider::OnMessage(const muduo::net::TcpConnectionPtr& conn,
                            muduo::net::Buffer* buffer,
                            muduo::Timestamp time)
{
    // 网络上接收的远程rpc调用请求的字节流数据
    std::string recv_buf = buffer->retrieveAllAsString(); 
    //获取字节流数据中header_size
    uint32_t header_size;
    recv_buf.copy((char*)&header_size,4,0);
    
    // 根据header_size得到header_str，并进行反序列化 
    std::string rpc_header_str = recv_buf.substr(4,header_size);
    mprpc::RpcHeader rpc_header;
    
    std::string service_name;
    std::string method_name;
    uint32_t args_size;

    if(rpc_header.ParseFromString(rpc_header_str))
    {
        // 数据头反序列化成功
        service_name = rpc_header.service_name();
        method_name = rpc_header.method_name();
        args_size =  rpc_header.args_size();
    }
    else
    {
        // 数据头反序列化失败
        LOG_ERROR("%s-%s-%d : rpc_header Parse false",__FILE__,__FUNCTION__,__LINE__);
        std::cout << "rpc_header Parse false" << std::endl;
        return; 
    }

    // 参数信息，进行反序列化  [debug] : 从rpc_header_str提取 
    std::string args_str = recv_buf.substr(4 + header_size,args_size);
    
    // 打印调试信息
    std::cout << "===============================" << std::endl;
    std::cout << "rpc service name : " << service_name << std::endl;
    std::cout << "rpc method name : " << method_name << std::endl;
    std::cout << "rpc args : " << args_str << std::endl;
    std::cout << "===============================" << std::endl;
    
    // 往映射表中查找注册的服务，和服务方法
    auto serviceinfo_it = m_serviceMap.find(service_name);
    if(serviceinfo_it == m_serviceMap.end()){
        LOG_ERROR("%s-%s-%d : service name : %s no find!",__FILE__,__FUNCTION__,__LINE__,service_name);
        std::cout << "[error] service name :" << service_name << " no find !" << std::endl;
        return;
    }

    auto method_it = serviceinfo_it->second.m_methodMap.find(method_name);
    if(method_it == serviceinfo_it->second.m_methodMap.end()){
        LOG_ERROR("%s-%s-%d : method name : %s no find!",__FILE__,__FUNCTION__,__LINE__,method_name);
        std::cout << "[error] method name :" << method_name <<" no find !" << std::endl;
        return;
    }

    // 获取请求服务对象和方法描述
    google::protobuf::Service *service_ptr = serviceinfo_it->second.m_service;
    const google::protobuf::MethodDescriptor* method_ptr = method_it->second;   
    
    // 根据请求服务对象和方法描述生成Rpc方法调用需要的Request和Response参数
    google::protobuf::Message *request = service_ptr->GetRequestPrototype(method_ptr).New();
    
    if(!request->ParseFromString(args_str)){
        std::cout << "Parse from args_str error" << std::endl;
        return;
    }

    // 用于method执行完业务逻辑后，框架将封装好的Response返回
    google::protobuf::Message *response = service_ptr->GetResponsePrototype(method_ptr).New();

    // 相当于结合使用std::bind和function使用,为CallMethod方法绑定回调Closure，调用Run函数则执行SendRpcResponse
    google::protobuf::Closure* done = google::protobuf::NewCallback
                                                        <RpcProvider,
                                                        const muduo::net::TcpConnectionPtr&,
                                                        google::protobuf::Message *
                                                        >
                                                        (this,&RpcProvider::SendRpcProvider,conn,response);
    

    // 调用method方法，传入已经将args_str序列化好的request和response
    service_ptr->CallMethod(method_ptr,nullptr,request,response,done);
    

}

void RpcProvider::SendRpcProvider(const muduo::net::TcpConnectionPtr& conn,google::protobuf::Message *response)
{
    std::string response_str;
    if(response->SerializeToString(&response_str)){
        conn->send(response_str);
    }else{
        std::cout << " Serialize RpcResponse str error " << std::endl;
    }
    // 模拟http的短链接，每次数据收发结束后，由RpcProvider主动断开连接
    std::cout << "连接断开" << " " << response_str << std::endl;
    conn->shutdown();
}
