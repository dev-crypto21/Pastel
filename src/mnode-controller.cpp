// Copyright (c) 2018 airk42
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "main.h"
#include "init.h"
#include "util.h"
#include "base58.h"
#include "ui_interface.h"

#include "mnode-controller.h"
#include "mnode-sync.h"
#include "mnode-manager.h"
#include "mnode-msgsigner.h"
#include "mnode-db.h"

#include <boost/lexical_cast.hpp>

constexpr const CNodeHelper::CFullyConnectedOnly CNodeHelper::FullyConnectedOnly;
constexpr const CNodeHelper::CAllNodes CNodeHelper::AllNodes;

/*
MasterNode specific logic and initializations
*/

CCriticalSection CMasterNodeController::cs_mapMasternodeBlocks;


void CMasterNodeController::SetParameters()
{
    MasternodeProtocolVersion           = 0x1;
    MasternodeCollateral                = 1000;

    MasternodeCheckSeconds              =   5;
    MasternodeMinMNBSeconds             =   5 * 60;
    MasternodeMinMNPSeconds             =  10 * 60;
    MasternodeExpirationSeconds         =  65 * 60;
    MasternodeWatchdogMaxSeconds        = 120 * 60;
    MasternodeNewStartRequiredSeconds   = 180 * 60;
    
    MasternodePOSEBanMaxScore           = 5;
    nMasterNodeMaximumOutboundConnections = 20;

    if (Params().IsMainNet()) {
        nMasternodeMinimumConfirmations = 15;
        nMasternodePaymentsStartBlock = 100000;
        nMasternodePaymentsIncreaseBlock = 150000;
        nMasternodePaymentsIncreasePeriod = 576*30;
        nFulfilledRequestExpireTime = 60*60; // 60 minutes
    }
    else if (Params().IsTestNet()) {
        nMasternodeMinimumConfirmations = 1;
        nMasternodePaymentsStartBlock = 4010;
        nMasternodePaymentsIncreaseBlock = 4030;
        nMasternodePaymentsIncreasePeriod = 10;
        nFulfilledRequestExpireTime = 5*60; // 5 minutes
    }
    else if (Params().IsRegTest()) {
        nMasternodeMinimumConfirmations = 1;
        nMasternodePaymentsStartBlock = 240;
        nMasternodePaymentsIncreaseBlock = 350;
        nMasternodePaymentsIncreasePeriod = 10;
        nFulfilledRequestExpireTime = 5*60; // 5 minutes

        MasternodeMinMNPSeconds             =  1 * 60;    
    }
    else{
        //TODO accert
    }
}


#ifdef ENABLE_WALLET
bool CMasterNodeController::EnableMasterNode(std::ostringstream& strErrors, boost::thread_group& threadGroup, CWallet* pWalletMain)
#else
bool CMasterNodeController::EnableMasterNode(std::ostringstream& strErrors, boost::thread_group& threadGroup)
#endif
{
    SetParameters();

    // parse masternode.conf
    std::string strErr;
    if(!masternodeConfig.read(strErr)) {
        fprintf(stderr,"Error reading masternode configuration file: %s\n", strErr.c_str());
        return false;
    }

    // NOTE: Masternode should have no wallet
    fMasterNode = GetBoolArg("-masternode", false);

    if((fMasterNode || masternodeConfig.getCount() > 0) && fTxIndex == false) {
        strErrors << _("Enabling Masternode support requires turning on transaction indexing.")
                << _("Please add txindex=1 to your configuration and start with -reindex");
        return false;
    }

    if(fMasterNode) {
        LogPrintf("MASTERNODE:\n");

        std::string strMasterNodePrivKey = GetArg("-masternodeprivkey", "");
        if(!strMasterNodePrivKey.empty()) {
            if(!CMessageSigner::GetKeysFromSecret(strMasterNodePrivKey, activeMasternode.keyMasternode, activeMasternode.pubKeyMasternode)) {
                strErrors << _("Invalid masternodeprivkey. Please see documentation.");
                return false;
            }

            LogPrintf("  pubKeyMasternode: %s\n", CBitcoinAddress(activeMasternode.pubKeyMasternode.GetID()).ToString());
        } else {
            strErrors << _("You must specify a masternodeprivkey in the configuration. Please see documentation for help.");
            return false;
        }
    }

#ifdef ENABLE_WALLET
    LogPrintf("Using masternode config file %s\n", GetMasternodeConfigFile().string());

    //Prevent Wallet from accidental spending of the collateral!!!
    if(GetBoolArg("-mnconflock", true) && pWalletMain && (masternodeConfig.getCount() > 0)) {
        LOCK(pWalletMain->cs_wallet);
        LogPrintf("Locking Masternodes:\n");
        uint256 mnTxHash;
        int outputIndex;
        BOOST_FOREACH(CMasternodeConfig::CMasternodeEntry mne, masternodeConfig.getEntries()) {
            mnTxHash.SetHex(mne.getTxHash());
            outputIndex = boost::lexical_cast<unsigned int>(mne.getOutputIndex());
            COutPoint outpoint = COutPoint(mnTxHash, outputIndex);
            // don't lock non-spendable outpoint (i.e. it's already spent or it's not from this wallet at all)
            if(pWalletMain->IsMine(CTxIn(outpoint)) != ISMINE_SPENDABLE) {
                LogPrintf("  %s %s - IS NOT SPENDABLE, was not locked\n", mne.getTxHash(), mne.getOutputIndex());
                continue;
            }
            pWalletMain->LockCoin(outpoint);
            LogPrintf("  %s %s - locked successfully\n", mne.getTxHash(), mne.getOutputIndex());
        }
    }
#endif // ENABLE_WALLET

    // LOAD SERIALIZED DAT FILES INTO DATA CACHES FOR INTERNAL USE

    boost::filesystem::path pathDB = GetDataDir();
    std::string strDBName;

    strDBName = "mncache.dat";
    uiInterface.InitMessage(_("Loading masternode cache..."));
    CFlatDB<CMasternodeMan> flatDB1(strDBName, "magicMasternodeCache");
    if(!flatDB1.Load(masternodeManager)) {
        strErrors << _("Failed to load masternode cache from") + "\n" + (pathDB / strDBName).string();
        return false;
    }

    if(masternodeManager.size()) {
        strDBName = "mnpayments.dat";
        uiInterface.InitMessage(_("Loading masternode payment cache..."));
/*TEMP-->
        CFlatDB<CMasternodePayments> flatDB2(strDBName, "magicMasternodePaymentsCache");
        if(!flatDB2.Load(masternodePayments)) {
            strErrors << _("Failed to load masternode payments cache from") + "\n" + (pathDB / strDBName).string();
            return false;
        }
<--TEMP*/
    } else {
        uiInterface.InitMessage(_("Masternode cache is empty, skipping payments and governance cache..."));
    }

    strDBName = "netfulfilled.dat";
    uiInterface.InitMessage(_("Loading fulfilled requests cache..."));
    CFlatDB<CMasternodeRequestTracker> flatDB3(strDBName, "magicFulfilledCache");
    if(!flatDB3.Load(requestTracker)) {
        strErrors << _("Failed to load fulfilled requests cache from") + "\n" + (pathDB / strDBName).string();
        return false;
    }

    // force UpdatedBlockTip to initialize nCachedBlockHeight for DS, MN payments and budgets
/*TEMP-->
    pdsNotificationInterface->InitializeCurrentBlockTip();
<--TEMP*/

    //Enable Maintenance thread
    threadGroup.create_thread(boost::bind(std::function<void()>(std::bind(&CMasterNodeController::ThreadMasterNodeMaintenance, this))));

    return true;
}

bool CMasterNodeController::StartMasterNode(boost::thread_group& threadGroup)
{
    if (semMasternodeOutbound == NULL) {
        // initialize semaphore
        semMasternodeOutbound = new CSemaphore(nMasterNodeMaximumOutboundConnections);
    }

    //Enable Broadcast re-requests thread
    threadGroup.create_thread(boost::bind(std::function<void()>(std::bind(&CMasterNodeController::ThreadMnbRequestConnections, this))));

}
bool CMasterNodeController::StopMasterNode()
{
    if (semMasternodeOutbound)
        for (int i=0; i<nMasterNodeMaximumOutboundConnections; i++)
            semMasternodeOutbound->post();

    delete semMasternodeOutbound;
    semMasternodeOutbound = NULL;
}


bool CMasterNodeController::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    masternodeManager.ProcessMessage(pfrom, strCommand, vRecv);
/*TEMP-->
    masternodePayments.ProcessMessage(pfrom, strCommand, vRecv);
<--TEMP*/
    masternodeSync.ProcessMessage(pfrom, strCommand, vRecv);

    return true;
}

bool CMasterNodeController::AlreadyHave(const CInv& inv)
{
    switch (inv.type)
    {
    case MSG_MASTERNODE_PAYMENT_VOTE:
        return masternodePayments.mapMasternodePaymentVotes.count(inv.hash);

    case MSG_MASTERNODE_PAYMENT_BLOCK:
        {
            BlockMap::iterator mi = mapBlockIndex.find(inv.hash);
            return mi != mapBlockIndex.end() && masternodePayments.mapMasternodeBlocks.find(mi->second->nHeight) != masternodePayments.mapMasternodeBlocks.end();
        }

    case MSG_MASTERNODE_ANNOUNCE:
        return masternodeManager.mapSeenMasternodeBroadcast.count(inv.hash) && !masternodeManager.IsMnbRecoveryRequested(inv.hash);

    case MSG_MASTERNODE_PING:
        return masternodeManager.mapSeenMasternodePing.count(inv.hash);

    case MSG_MASTERNODE_VERIFY:
        return masternodeManager.mapSeenMasternodeVerification.count(inv.hash);
    };

    return true;
}

bool CMasterNodeController::ProcessGetData(CNode* pfrom, const CInv& inv)
{
    bool pushed = false;

    if (!pushed && inv.type == MSG_MASTERNODE_PAYMENT_VOTE) {
        if(masternodePayments.HasVerifiedPaymentVote(inv.hash)) {
            CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
            ss.reserve(1000);
/*TEMP-->
            ss << masternodePayments.mapMasternodePaymentVotes[inv.hash];
<--TEMP*/
            pfrom->PushMessage(NetMsgType::MASTERNODEPAYMENTVOTE, ss);
            pushed = true;
        }
    }

    if (!pushed && inv.type == MSG_MASTERNODE_PAYMENT_BLOCK) {
        BlockMap::iterator mi = mapBlockIndex.find(inv.hash);
        LOCK(cs_mapMasternodeBlocks);
        if (mi != mapBlockIndex.end() && masternodePayments.mapMasternodeBlocks.count(mi->second->nHeight)) {
            BOOST_FOREACH(CMasternodePayee& payee, masternodePayments.mapMasternodeBlocks[mi->second->nHeight].vecPayees) {
                std::vector<uint256> vecVoteHashes = payee.GetVoteHashes();
                BOOST_FOREACH(uint256& hash, vecVoteHashes) {
                    if(masternodePayments.HasVerifiedPaymentVote(hash)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
/*TEMP-->
                        ss << masternodePayments.mapMasternodePaymentVotes[hash];
<--TEMP*/
                        pfrom->PushMessage(NetMsgType::MASTERNODEPAYMENTVOTE, ss);
                    }
                }
            }
            pushed = true;
        }
    }

    if (!pushed && inv.type == MSG_MASTERNODE_ANNOUNCE) {
        if(masternodeManager.mapSeenMasternodeBroadcast.count(inv.hash)){
            CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
            ss.reserve(1000);
            ss << masternodeManager.mapSeenMasternodeBroadcast[inv.hash].second;
            pfrom->PushMessage(NetMsgType::MNANNOUNCE, ss);
            pushed = true;
        }
    }

    if (!pushed && inv.type == MSG_MASTERNODE_PING) {
        if(masternodeManager.mapSeenMasternodePing.count(inv.hash)) {
            CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
            ss.reserve(1000);
            ss << masternodeManager.mapSeenMasternodePing[inv.hash];
            pfrom->PushMessage(NetMsgType::MNPING, ss);
            pushed = true;
        }
    }
    return pushed;
}

void CMasterNodeController::ShutdownMasterNode()
{
    // STORE DATA CACHES INTO SERIALIZED DAT FILES
    CFlatDB<CMasternodeMan> flatDB1("mncache.dat", "magicMasternodeCache");
    flatDB1.Dump(masternodeManager);
/*TEMP-->
    CFlatDB<CMasternodePayments> flatDB2("mnpayments.dat", "magicMasternodePaymentsCache");
    flatDB2.Dump(masternodePayments);
<--TEMP*/
    CFlatDB<CMasternodeRequestTracker> flatDB3("netfulfilled.dat", "magicFulfilledCache");
    flatDB3.Dump(requestTracker);
}

boost::filesystem::path CMasterNodeController::GetMasternodeConfigFile()
{
    boost::filesystem::path pathConfigFile(GetArg("-mnconf", "masternode.conf"));
    if (!pathConfigFile.is_complete()) pathConfigFile = GetDataDir() / pathConfigFile;
    return pathConfigFile;
}

/*
Wrappers for BlockChain specific logic
*/

CAmount CMasterNodeController::GetMasternodePayment(int nHeight, CAmount blockValue)
{
    CAmount ret = blockValue/5; // start at 20%

    int nMNPIBlock = nMasternodePaymentsIncreaseBlock;
    int nMNPIPeriod = nMasternodePaymentsIncreasePeriod;

                                                                      // mainnet:
    if(nHeight > nMNPIBlock)                  ret += blockValue / 20; // 158000 - 25.0% - 2014-10-24
    if(nHeight > nMNPIBlock+(nMNPIPeriod* 1)) ret += blockValue / 20; // 175280 - 30.0% - 2014-11-25
    if(nHeight > nMNPIBlock+(nMNPIPeriod* 2)) ret += blockValue / 20; // 192560 - 35.0% - 2014-12-26
    if(nHeight > nMNPIBlock+(nMNPIPeriod* 3)) ret += blockValue / 40; // 209840 - 37.5% - 2015-01-26
    if(nHeight > nMNPIBlock+(nMNPIPeriod* 4)) ret += blockValue / 40; // 227120 - 40.0% - 2015-02-27
    if(nHeight > nMNPIBlock+(nMNPIPeriod* 5)) ret += blockValue / 40; // 244400 - 42.5% - 2015-03-30
    if(nHeight > nMNPIBlock+(nMNPIPeriod* 6)) ret += blockValue / 40; // 261680 - 45.0% - 2015-05-01
    if(nHeight > nMNPIBlock+(nMNPIPeriod* 7)) ret += blockValue / 40; // 278960 - 47.5% - 2015-06-01
    if(nHeight > nMNPIBlock+(nMNPIPeriod* 9)) ret += blockValue / 40; // 313520 - 50.0% - 2015-08-03

    return ret;
}

/* static */ bool CMasterNodeController::GetBlockHash(uint256& hashRet, int nBlockHeight)
{
    LOCK(cs_main);
    if(chainActive.Tip() == NULL) return false;
    if(nBlockHeight < -1 || nBlockHeight > chainActive.Height()) return false;
    if(nBlockHeight == -1) nBlockHeight = chainActive.Height();
    hashRet = chainActive[nBlockHeight]->GetBlockHash();
    return true;
}

/* static */ bool CMasterNodeController::GetUTXOCoin(const COutPoint& outpoint, CCoins& coins)
{
    LOCK(cs_main);
    if (!pcoinsTip->GetCoins(outpoint.hash, coins))
        return false;
    if (coins.vout[outpoint.n].IsNull())
        return false;
    return true;
}

/* static */ int CMasterNodeController::GetUTXOHeight(const COutPoint& outpoint)
{
    // -1 means UTXO is yet unknown or already spent
    CCoins coins;
    return GetUTXOCoin(outpoint, coins) ? coins.nHeight : -1;
}

/* static */ int CMasterNodeController::GetUTXOConfirmations(const COutPoint& outpoint)
{
    // -1 means UTXO is yet unknown or already spent
    LOCK(cs_main);
    int nPrevoutHeight = GetUTXOHeight(outpoint);
    return (nPrevoutHeight > -1 && chainActive.Tip()) ? chainActive.Height() - nPrevoutHeight + 1 : -1;
}

#ifdef ENABLE_WALLET
/* static */ bool CMasterNodeController::GetMasternodeOutpointAndKeys(CWallet* pWalletMain, COutPoint& outpointRet, CPubKey& pubKeyRet, CKey& keyRet, std::string strTxHash, std::string strOutputIndex)
{
    // wait for reindex and/or import to finish
    if (fImporting || fReindex || pWalletMain == NULL) return false;

    // Find possible candidates
    std::vector<COutput> vPossibleCoins;
    pWalletMain->AvailableCoins(vPossibleCoins, true, NULL, false, true, masterNodeCtrl.MasternodeCollateral, true);
    if(vPossibleCoins.empty()) {
        LogPrintf("CMasterNodeController::GetMasternodeOutpointAndKeys -- Could not locate any valid masternode vin\n");
        return false;
    }

    if(strTxHash.empty()) // No output specified, select the first one
        return GetOutpointAndKeysFromOutput(pWalletMain, vPossibleCoins[0], outpointRet, pubKeyRet, keyRet);

    // Find specific vin
    uint256 txHash = uint256S(strTxHash);
    int nOutputIndex = atoi(strOutputIndex.c_str());

    BOOST_FOREACH(COutput& out, vPossibleCoins)
        if(out.tx->GetHash() == txHash && out.i == nOutputIndex) // found it!
            return GetOutpointAndKeysFromOutput(pWalletMain, out, outpointRet, pubKeyRet, keyRet);

    LogPrintf("CMasterNodeController::GetMasternodeOutpointAndKeys -- Could not locate specified masternode vin\n");
    return false;
}

/* static */ bool CMasterNodeController::GetOutpointAndKeysFromOutput(CWallet* pWalletMain, const COutput& out, COutPoint& outpointRet, CPubKey& pubKeyRet, CKey& keyRet)
{
    // wait for reindex and/or import to finish
    if (fImporting || fReindex || pWalletMain == NULL) return false;

    CScript pubScript;

    outpointRet = COutPoint(out.tx->GetHash(), out.i);
    pubScript = out.tx->vout[out.i].scriptPubKey; // the inputs PubKey

    CTxDestination address1;
    ExtractDestination(pubScript, address1);
    CBitcoinAddress address2(address1);

    CKeyID keyID;
    if (!address2.GetKeyID(keyID)) {
        LogPrintf("CMasterNodeController::GetOutpointAndKeysFromOutput -- Address does not refer to a key\n");
        return false;
    }

    if (!pWalletMain->GetKey(keyID, keyRet)) {
        LogPrintf ("CMasterNodeController::GetOutpointAndKeysFromOutput -- Private key for address is not known\n");
        return false;
    }

    pubKeyRet = keyRet.GetPubKey();
    return true;
}
#endif

/*
Threads
*/

void CMasterNodeController::ThreadMnbRequestConnections()
{
    RenameThread("animecoin-mn-mnbreq");

    // Connecting to specific addresses, no masternode connections available
    if (mapArgs.count("-connect") && mapMultiArgs["-connect"].size() > 0)
        return;

    while (true)
    {
        MilliSleep(500);

        CSemaphoreGrant grant(*semMasternodeOutbound);

        std::pair<CService, std::set<uint256> > p = masterNodeCtrl.masternodeManager.PopScheduledMnbRequestConnection();
        if(p.first == CService() || p.second.empty()) continue;

        ConnectNode(CAddress(p.first, NODE_NETWORK), NULL, true);

        LOCK(cs_vNodes);

        CNode *pnode = FindNode(p.first);
        if(!pnode || pnode->fDisconnect) continue;

        grant.MoveTo(pnode->grantMasternodeOutbound);

        // compile request vector
        std::vector<CInv> vToFetch;
        std::set<uint256>::iterator it = p.second.begin();
        while(it != p.second.end()) {
            if(*it != uint256()) {
                vToFetch.push_back(CInv(MSG_MASTERNODE_ANNOUNCE, *it));
                LogPrint("masternode", "ThreadMnbRequestConnections -- asking for mnb %s from addr=%s\n", it->ToString(), p.first.ToString());
            }
            ++it;
        }

        // ask for data
        pnode->PushMessage("getdata", vToFetch);
    }
}

void CMasterNodeController::ThreadMasterNodeMaintenance()
{
    static bool fOneThread;
    if(fOneThread) return;
    fOneThread = true;

    RenameThread("animecoin-mn");

    unsigned int nTick = 0;

    while (true)
    {
        MilliSleep(1000);

        // try to sync from all available nodes, one step at a time
        masterNodeCtrl.masternodeSync.ProcessTick();

        if(masterNodeCtrl.masternodeSync.IsBlockchainSynced() && !ShutdownRequested()) {

            nTick++;

            // make sure to check all masternodes first
            masterNodeCtrl.masternodeManager.Check();

            // check if we should activate or ping every few minutes,
            // slightly postpone first run to give net thread a chance to connect to some peers
            if(nTick % masterNodeCtrl.MasternodeMinMNPSeconds == 15)
                masterNodeCtrl.activeMasternode.ManageState();

            if(nTick % 60 == 0) {
                masterNodeCtrl.masternodeManager.ProcessMasternodeConnections();
                masterNodeCtrl.masternodeManager.CheckAndRemove();
/*TEMP-->
                masterNodeCtrl.masternodePayments.CheckAndRemove();
<--TEMP*/
            }
            if(masterNodeCtrl.IsMasterNode() && (nTick % (60 * 5) == 0)) {
                masterNodeCtrl.masternodeManager.DoFullVerificationStep();
            }
        }
    }
}