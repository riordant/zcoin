// Copyright (c) 2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "chainparams.h"
#include "chain.h"
#include "zmqpublishnotifier.h"
#include "main.h"
#include "util.h"
#include "rpc/server.h"
#include "script/standard.h"
#include "base58.h"
#include "client-api/json.hpp"
#include "client-api/zmq.h"
#include "znode-sync.h"
#include "net.h"

using path = boost::filesystem::path;
using json = nlohmann::json;
using namespace std::chrono;

static std::multimap<std::string, CZMQAbstractPublishNotifier*> mapPublishNotifiers;

static const char *MSG_HASHBLOCK = "hashblock";
static const char *MSG_HASHTX    = "hashtx";
//static const char *MSG_RAWBLOCK  = "rawblock";
//static const char *MSG_RAWTX     = "rawtx";

void *psocket;

// Internal function to send multipart message
static int zmq_send_multipart(void *sock, const void* data, size_t size, ...)
{
    va_list args;
    va_start(args, size);

    while (1)
    {
        zmq_msg_t msg;

        int rc = zmq_msg_init_size(&msg, size);
        if (rc != 0)
        {
            zmqError("Unable to initialize ZMQ msg");
            return -1;
        }

        void *buf = zmq_msg_data(&msg);
        memcpy(buf, data, size);

        data = va_arg(args, const void*);

        rc = zmq_msg_send(&msg, sock, data ? ZMQ_SNDMORE : 0);
        if (rc == -1)
        {
            zmqError("Unable to send ZMQ msg");
            zmq_msg_close(&msg);
            return -1;
        }

        LogPrintf("ZMQ: message sent.\n");

        zmq_msg_close(&msg);

        if (!data)
            break;

        size = va_arg(args, size_t);
    }
    return 0;
}

bool CZMQAbstractPublishNotifier::Initialize(void *pcontext)
{
    assert(!psocket);

    // check if address is being used by other publish notifier
    std::multimap<std::string, CZMQAbstractPublishNotifier*>::iterator i = mapPublishNotifiers.find(address);

    if (i==mapPublishNotifiers.end())
    {
        psocket = zmq_socket(pcontext, ZMQ_PUB);
        if (!psocket)
        {
            zmqError("Failed to create socket");
            return false;
        }

        if(DEV_AUTH){
            // Set up PUB auth.
            vector<string> keys = read_cert("server");

            string server_secret_key = keys.at(1);

            const int curve_server_enable = 1;
            zmq_setsockopt(psocket, ZMQ_CURVE_SERVER, &curve_server_enable, sizeof(curve_server_enable));
            zmq_setsockopt(psocket, ZMQ_CURVE_SECRETKEY, server_secret_key.c_str(), 40);
        }

        int rc = zmq_bind(psocket, address.c_str());
        if (rc!=0)
        {
            zmqError("Failed to bind address");
            zmq_close(psocket);
            return false;
        }

        // register this notifier for the address, so it can be reused for other publish notifier
        mapPublishNotifiers.insert(std::make_pair(address, this));
        return true;
    }
    else
    {
        LogPrint(NULL, "zmq: Reusing socket for address %s\n", address);

        psocket = i->second->psocket;
        mapPublishNotifiers.insert(std::make_pair(address, this));

        return true;
    }
}

void CZMQAbstractPublishNotifier::Shutdown()
{
    assert(psocket);

    int count = mapPublishNotifiers.count(address);

    // remove this notifier from the list of publishers using this address
    typedef std::multimap<std::string, CZMQAbstractPublishNotifier*>::iterator iterator;
    std::pair<iterator, iterator> iterpair = mapPublishNotifiers.equal_range(address);

    for (iterator it = iterpair.first; it != iterpair.second; ++it)
    {
        if (it->second==this)
        {
            mapPublishNotifiers.erase(it);
            break;
        }
    }

    if (count == 1)
    {
        LogPrint(NULL, "Close socket at address %s\n", address);
        int linger = 0;
        zmq_setsockopt(psocket, ZMQ_LINGER, &linger, sizeof(linger));
        zmq_close(psocket);
    }

    psocket = 0;
}

bool CZMQAbstractPublishNotifier::SendTopicMessage(const char *command, const void* data, size_t size)
{
    assert(psocket);

    LogPrintf("zmq: in SendMessage\n");

    /* send three parts, command & data & a LE 4byte sequence number */
    unsigned char msgseq[sizeof(uint32_t)];
    WriteLE32(&msgseq[0], nSequence);
    int rc = zmq_send_multipart(psocket, command, strlen(command), data, size, msgseq, (size_t)sizeof(uint32_t), (void*)0);
    if (rc == -1)
        return false;

    /* increment memory only sequence number after sending */
    nSequence++;

    return true;
}


bool CZMQAbstractPublishNotifier::SendMessage(string msg){

    LogPrintf("ZMQ: sending message %s\n", msg);
    assert(psocket);

    zmq_msg_t reply;
    int rc = zmq_msg_init_size (&reply, msg.size());
    assert(rc == 0);  
    std::memcpy (zmq_msg_data (&reply), msg.data(), msg.size());
    //LogPrintf("ZMQ: Sending reply..\n");
    /* Block until a message is available to be sent from socket */
    rc = zmq_sendmsg (psocket, &reply, 0);
    assert(rc!=-1);

    LogPrintf("ZMQ: message sent.\n");
    zmq_msg_close(&reply);

    return true;
}



/***************** not used *******************/
bool CZMQPublishHashBlockNotifier::NotifyBlock(const CBlockIndex *pindex)
{
    uint256 hash = pindex->GetBlockHash();
    LogPrint(NULL, "zmq: Publish hashblock %s\n", hash.GetHex());
    char data[32];
    for (unsigned int i = 0; i < 32; i++)
        data[31 - i] = hash.begin()[i];
    return SendTopicMessage(MSG_HASHBLOCK, data, 32);
}

bool CZMQPublishHashTransactionNotifier::NotifyTransaction(const CTransaction &transaction)
{
    uint256 hash = transaction.GetHash();
    LogPrint(NULL, "zmq: Publish hashtx %s\n", hash.GetHex());
    char data[32];
    for (unsigned int i = 0; i < 32; i++)
        data[31 - i] = hash.begin()[i];
    return SendTopicMessage(MSG_HASHTX, data, 32);
}
/***************** not used *******************/



bool CZMQPublishRawTransactionNotifier::NotifyTransaction(const CTransaction &transaction)
{
    /*
    address publishing layout for new tx's:
        {
            "type": "address",
            "id": STRING,
            "transaction": {
                "txid": STRING,
                "timestamp": INT (created here & changed to block timestamp with 6 confs)
                "amount": INT
                "type": type: 'in|out|mint|spend|mining|znode'
            }
        }
    */
    LogPrintf("ZMQ: in NotifyTransaction\n");
    LogPrintf("ZMQ: blockchain synced: %s\n", znodeSync.IsBlockchainSynced());
    if(znodeSync.IsBlockchainSynced()){
        processTransaction(transaction);
    }
    
    return true;
}

bool CZMQPublishRawBlockNotifier::NotifyBlock(const CBlockIndex *pindex)
{
    //publish block related info every 10 blocks.
    int currentHeight = pindex->nHeight;
    string topic;
    string message;
    LogPrintf("ZMQ: in notifyblock. currentHeight: %s\n", to_string(currentHeight));
    bool syncing = (currentHeight % 10==0 && currentHeight >=10);
    string prevblockhash;
    if(syncing || znodeSync.IsBlockchainSynced()){
        // if blockchain synced - get every block. if not get 10 previous blocks every 10
        if(znodeSync.IsBlockchainSynced()){
            prevblockhash = pindex->GetBlockHash().ToString();
        }else {
            prevblockhash = chainActive[currentHeight - (syncing ? 10 : 0)]->GetBlockHash().ToString();
        }
        LogPrintf("ZMQ: prevblockhash: %s\n", prevblockhash);

        json result_json;
        result_json = WalletDataSinceBlock(prevblockhash);

        topic = "address";
        message = result_json.dump();
        SendTopicMessage(topic.c_str(), message.c_str(), message.length());
    }

    //publish Blockchain related info.
    json block_json;
    block_json["type"] = "full";
    block_json["status"] = nullptr;

    block_json["status"]["IsBlockchainSynced"] = znodeSync.IsBlockchainSynced();
    block_json["status"]["IsZnodeListSynced"] = znodeSync.IsZnodeListSynced();
    block_json["status"]["IsWinnersListSynced"] = znodeSync.IsWinnersListSynced();
    block_json["status"]["IsSynced"] = znodeSync.IsSynced();
    block_json["status"]["IsFailed"] = znodeSync.IsFailed();

    block_json["testnet"] = Params().TestnetToBeDeprecatedFieldRPC();

    block_json["connections"] = (int)vNodes.size();

    block_json["currentBlock"] = nullptr;
    block_json["currentBlock"]["height"] = pindex->nHeight;
    block_json["currentBlock"]["timestamp"] = pindex->nTime;

    json response = finalize_json(block_json, false);

    topic = "block";
    message = response.dump();
    SendTopicMessage(topic.c_str(), message.c_str(), message.length());

    return true;
}

bool CZMQAbstractPublishNotifier::writeTimestampToFile(json tx){
    //get payment request data
    path persistent_pr = GetDataDir(true) / "persistent" / "tx-timestamp.json";

    // get raw string
    std::ifstream persistent_pr_in(persistent_pr.string());

    // convert to JSON
    json persistent_pr_json;
    persistent_pr_in >> persistent_pr_json;

    // get "data" object from JSON
    json data_json = persistent_pr_json["data"];

    string txid = tx["transaction"]["txid"];
    int timestamp = tx["transaction"]["timestamp"];

    data_json[txid] = timestamp;

    // write request back to JSON
    persistent_pr_json["data"] = data_json;
        
    // write back to file.
    std::ofstream persistent_pr_out(persistent_pr.string());
    persistent_pr_out << std::setw(4) << persistent_pr_json << std::endl;

    return true;
}

bool CZMQAbstractPublishNotifier::processTransaction(const CTransaction &transaction){

    // // get time in ms
    milliseconds ms = duration_cast< milliseconds >(
      system_clock::now().time_since_epoch()
    );

    string topic = "address";
    string message;

    string txid = transaction.GetHash().ToString();

    json result;
        
    // handle tx_outs
    for (unsigned long i=0; i < transaction.vout.size(); i++) {
        //extract address(es) related to this vout
        CScript scriptPubKey = transaction.vout[i].scriptPubKey;
        vector<string> addresses;   
        vector<CTxDestination> addresses_raw;
        txnouttype type;
        int nRequired;

        ExtractDestinations(scriptPubKey, type, addresses_raw, nRequired);
        BOOST_FOREACH(const CTxDestination& tx_dest, addresses_raw)
            addresses.push_back(CBitcoinAddress(tx_dest).ToString());

        for(unsigned long j=0;j<addresses.size();j++){
             string new_address = addresses[j];
             result["data"][new_address]["txids"][txid]["category"] = nullptr;

             string category = "in";
             if(transaction.IsCoinBase() && transaction.vout[i].nValue==15 * COIN){
                category = "znode";
             }
             else if(transaction.IsCoinBase() && transaction.vout[i].nValue>=28 * COIN){
                category = "mined";
             }
             else if(transaction.IsZerocoinMint()){
                category = "mint";
             }
            else if(transaction.IsZerocoinSpend()){
                category = "spend";
             }
             
             json tx;
             tx["address"] = new_address;
             tx["amount"] = transaction.vout[i].nValue;
             tx["category"] = category;
             tx["confirmations"];
             tx["fee"];
             tx["time"] = ms.count();
             tx["txid"] = txid;

             result["data"][new_address]["txids"][txid]["category"][category] = tx;


             message = result.dump();

             SendTopicMessage(topic.c_str(), message.c_str(), message.length());
        }
    } 
    return true;

}