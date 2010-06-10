/*
	Flexisip, a flexible SIP proxy server with media capabilities.
    Copyright (C) 2010  Belledonne Communications SARL.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "transcodeagent.hh"
#include "offeranswer.h"
#include "sdp-modifier.hh"

CallSide::CallSide(){
	mSession=rtp_session_new(RTP_SESSION_SENDRECV);
	mEncoder=NULL;
	mDecoder=NULL;
	mReceiver=ms_filter_new(MS_RTP_RECV_ID);
	mSender=ms_filter_new(MS_RTP_SEND_ID);
}

CallSide::~CallSide(){
	rtp_session_destroy(mSession);
	ms_filter_destroy(mReceiver);
	ms_filter_destroy(mSender);
	if (mEncoder)
		ms_filter_destroy(mEncoder);
	if (mSender)
		ms_filter_destroy(mDecoder);
}

PayloadType *CallSide::getSendFormat(){
	int pt=rtp_session_get_send_payload_type(mSession);
	RtpProfile *prof=rtp_session_get_send_profile(mSession);
	return rtp_profile_get_payload(prof,pt);
}

MSConnectionPoint CallSide::getRecvPoint(){
	MSConnectionPoint ret;
	ret.filter=mReceiver;
	ret.pin=0;
	return ret;
}

PayloadType * CallSide::getRecvFormat(){
	int pt=rtp_session_get_recv_payload_type(mSession);
	RtpProfile *prof=rtp_session_get_recv_profile(mSession);
	return rtp_profile_get_payload(prof,pt);
}

void CallSide::connect(CallSide *recvSide){
	MSConnectionHelper h;
	PayloadType *recvpt;
	PayloadType *sendpt;

	recvpt=recvSide->getRecvFormat();
	sendpt=getSendFormat();
	ms_connection_helper_start(&h);
	ms_connection_helper_link(&h,recvSide->getRecvPoint().filter,-1,
	                          recvSide->getRecvPoint().pin);
	
	if (strcasecmp(recvpt->mime_type,sendpt->mime_type)!=0
	    && recvpt->clock_rate!=sendpt->clock_rate){
		mDecoder=ms_filter_create_decoder(recvpt->mime_type);
		mEncoder=ms_filter_create_encoder(sendpt->mime_type);
	}
	if (mDecoder)
		ms_connection_helper_link(&h,mDecoder,0,0);
	if (mEncoder)
		ms_connection_helper_link(&h,mEncoder,0,0);
	ms_connection_helper_link(&h,mSender,0,-1);
}

void CallSide::disconnect(CallSide *recvSide){
	MSConnectionHelper h;

	ms_connection_helper_start(&h);
	ms_connection_helper_unlink(&h,recvSide->getRecvPoint().filter,-1,
	                            	recvSide->getRecvPoint().pin);
	if (mDecoder)
		ms_connection_helper_unlink(&h,mDecoder,0,0);
	if (mEncoder)
		ms_connection_helper_unlink(&h,mEncoder,0,0);
	ms_connection_helper_unlink(&h,mSender,0,-1);
}

CallContext::CallContext(sip_t *sip) : CallContextBase(sip){
	su_home_init(&mHome);
}

void CallContext::join(MSTicker *t){
	mFrontSide.connect(&mBackSide);
	mBackSide.connect(&mFrontSide);
	ms_ticker_attach(t,mFrontSide.getRecvPoint().filter);
	ms_ticker_attach(t,mBackSide.getRecvPoint().filter);
	mTicker=t;
}

void CallContext::unjoin(){
	ms_ticker_detach(mTicker,mFrontSide.getRecvPoint().filter);
	ms_ticker_detach(mTicker,mFrontSide.getRecvPoint().filter);
	mFrontSide.disconnect(&mBackSide);
	mBackSide.disconnect(&mFrontSide);
}

CallContext::~CallContext(){
	su_home_deinit(&mHome);
}

static MSList *makeSupportedAudioPayloadList(){
	payload_type_set_number(&payload_type_pcmu8000,0);
	payload_type_set_number(&payload_type_pcma8000,8);
	payload_type_set_number(&payload_type_gsm,3);
	payload_type_set_number(&payload_type_speex_nb,-1);
	payload_type_set_number(&payload_type_speex_wb,-1);
	payload_type_set_number(&payload_type_amr,-1);
	MSList *l=ms_list_append(NULL,&payload_type_pcmu8000);
	l=ms_list_append(l,&payload_type_pcma8000);
	l=ms_list_append(l,&payload_type_gsm);
	l=ms_list_append(l,&payload_type_speex_nb);
	l=ms_list_append(l,&payload_type_speex_wb);
	l=ms_list_append(l,&payload_type_amr);
	return l;
}

TranscodeAgent::TranscodeAgent(su_root_t *root, const char *locaddr, int port) :
	Agent(root,locaddr,port){
	ortp_init();
	ms_init();
	mTicker=ms_ticker_new();
	mSupportedAudioPayloads=makeSupportedAudioPayloadList();
}

TranscodeAgent::~TranscodeAgent(){
	ms_ticker_destroy(mTicker);
}

void TranscodeAgent::processNewInvite(CallContext *c, msg_t *msg, sip_t *sip){
	SdpModifier *m=SdpModifier::createFromSipMsg(c->getHome(), sip);
	m->changeAudioIpPort (getLocAddr().c_str(),getPort());
	m->appendNewPayloads (mSupportedAudioPayloads);
	m->update(msg,sip);
	delete m;
	Agent::onRequest(msg,sip);
}

int TranscodeAgent::onRequest(msg_t *msg, sip_t *sip){
	CallContext *c;
	if (sip->sip_request->rq_method==sip_method_invite){
		if ((c=static_cast<CallContext*>(mCalls.find(sip)))==NULL){
			c=new CallContext(sip);
			mCalls.store(c);
			processNewInvite(c,msg,sip);
		}else{
			if (c->isNewInvite(sip)){
				processNewInvite(c,msg,sip);
			}else
				LOGW("Invite retransmission, not handled yet");
		}
	}else{
		//all other requests go through
		Agent::onRequest(msg,sip);
	}
	return 0;
}

void TranscodeAgent::process200Ok(CallContext *c, msg_t *msg, sip_t *sip){
}

int TranscodeAgent::onResponse(msg_t *msg, sip_t *sip){
	if (sip->sip_status->st_status==200 && sip->sip_cseq 
	    && sip->sip_cseq->cs_method==sip_method_invite){
		
	}
	return Agent::onResponse(msg,sip);
}


