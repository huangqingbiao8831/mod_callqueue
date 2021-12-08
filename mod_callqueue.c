/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2021, qingbiao huang II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * qingbiao huang II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * qingbiao huang II <anthm@freeswitch.org>
 *
 *
 * mod_callqueue.c -- Framework call queue from amqp Module
 *
 */
#include <switch.h>
#define TORTURE_ME
#define CQ_DESC "callqueue"
#define CQ_USAGE "queue_from_acd"
#define MAX_QUEUE_LEN 100000
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_callqueue_shutdown);
SWITCH_MODULE_LOAD_FUNCTION(mod_callqueue_load);
SWITCH_MODULE_DEFINITION(mod_callqueue, mod_callqueue_load, mod_callqueue_shutdown, NULL);
static const char *global_cf = "callqueue.conf"; //配置文件

#define MY_EVENT_COOL   "test::cool"
#define IR_EVENT_REQ    "ir::req"
#define CC_EVENT_REQ    "cc::req"
#define IR_EVENT_RES    "ir::resp"
#define CC_EVENT_RES    "cc::resp" 
#define CC_EVENT_CANCEL "cc::cancel"
#define IR_EVENT_CANCEL "ir::cancel"
#define CALLQUEUE_TIMEOUT  "cq::timeout"

#define SESSION_TIMEOUT_EVENT  "SessionTimeout"
#define ONE_REQ_TIMEOUT_EVENT  "OneReqTimeout"

//最大主被叫号码长度
#define MAX_USER_NUM     32

//消息的响应类型
#define IR_RES_ACK                    0x00
#define IR_RES_SUCCESS                0x01
#define IR_RES_FULL                   0x02
#define IR_RES_AGENT_DISTRIBUTE       0x03
#define IR_RES_QUEUE_BUSY             0x04
#define IR_RES_QUEUE_CANCEL_SUCCESS   0x05
#define IR_RES_QUEUE_ERROR            0x06
#define IR_RES_NO_AGENT_IN            0x07
#define IR_RES_HOW_MAY_WAIT           0x08
#define CC_RES_AGENT_BUSY             0x10
#define DTMF_CANCEL_REQ               0x20
#define DTMF_CONTINUE_REQ             0x21
#define TIME_OUT_EVENT                0x31
#define ONE_SESSION_TIMEOUT           0x32
#define ONE_REQ_TIMEOUT               0x33

//#define TORTURE_ME
//排队的状态机...
/***********************************************************************************************************
1.QUEUE_INIT:初始化状态，发送ir::req消息后，状态迁移到QUEUE_IR_REQ状态.
2.QUEUE_IR_REQ:由发送ir::req消息迁移过来...
3.QUEUE_IR_SUCCESS:收到ir::resp排队成功消息，由Queue_iR_seccess迁移过来。
4.QUEUE_DISTRIBUTE_SUCCESS:收到ir::resp分配坐席成功消息,由QUEUE_IR_SUCCESS状态迁移过来。
5.QUEUE_DISTRIBUTE_ERROR:收到ir::resp分配坐席失败消息，由QUEUE_IR_SUCCESS状态迁移过来.
6.QUEUE_CC_REQ:分配坐席成功后，向cc成功发送了cc::req消息，由QUEUE_DISTRIBUTE_SUCCESS迁移过来。
7.QUEUE_CC_CANCEL:处于QUEUE_CC_REQ该状态，用户主动通过dtmf取消，或者cc响应cc::resp，由QUEUE_CC_REQ迁移进入。
8.QUEUE_TIMEOUT：设置一个超时状态，从最近发送一个ir::req消息到当前时间，如果出现超过设置的时间，则状态就迁移到超时状态。
其它说明：
    在QUEUE_DISTRIBUTE_ERROR状态时，可以根据用户的请求，重新进入QUEUE_INIT状态，从而进行下一轮的排队。
************************************************************************************************************/
typedef enum {
	QUEUE_INIT_S,           
	QUEUE_IR_REQ_S,         
	QUEUE_IR_SUCCESS_S,     
	QUEUE_DISTRIBUTE_ERROR_S,   
	QUEUE_CC_REQ_S,             
	QUEUE_CC_CANCEL_S,
	QUEUE_TIMEOUT_S
} queue_status_t;
//队列状态表
typedef struct{
	const char *name;
	int state;
}queue_state_table_t;



/*
static queue_state_table_t QUEUE_STATE_CHART[] = {
	{"init", QUEUE_INIT},
	{"queue successfull", QUEUE_SUCCESSFUL},
	{"agent full", QUEUE_AGENT_FULL},
	{"agent distribute", QUEUE_AGENT_DISTRIBUTE},
	{"agent busy", QUEUE_AGENT_BUSY},
	{"cancel successful", QUEUE_CANCEL_SUCCESS},
	{"queue error", QUEUE_SELF_ERROR},
	{"no agent in", QUEUE_NO_AGENT},
	{"queue agent num", QUEUE_WAIT_NUM},
	{"user exit self", QUEUE_USER_BREAK},
	{NULL, 0}

};
*/
//配置信息...
static struct {
	int debug;
	int32_t threads;
	int32_t running;

    int32_t total_queue_counter;  //总的排队次数

	char *init_voice; /*背景音乐*/
	char *success_voice;/*进入排队成功提示*/
	char *full_voice;   /*队列满提示*/
	char *queue_busy_voice; /*队列忙提示*/
	char *agent_busy_voice; /*分配坐席，接续坐席时，坐席忙提示*/
	char *agent_succ_voice; /*分配坐席成功提示...*/
	char *cancel_voice;     /*用户取消排队提示...*/
	char *error_voice;      /*排队中发生错误提示...*/
	char *no_angent_voice;  /*没有坐席签入提示*/
	char *queue_session_timeout_voice; /*排队时间过程提示结束排队...*/
	char *wait_number_voice;
    char *user_break_voice;

	uint32_t max_queue_session_time; /*整体排队最长等待时间，以秒为单位*/
    uint32_t max_one_queue_wait_time; /*记录单次排队请求到排队结束时间,以秒为单位*/
	uint32_t max_queue_times;         /*一次排队的最大排队请求次数*/
	switch_mutex_t *mutex;
	switch_memory_pool_t *pool;
	switch_mutex_t *read_mutex;  /*queue_hash信号量...*/
	switch_hash_t *queue_hash;   /*记录当前排队的session id和处理的对应关系，以便于给处理队列中发送消息...*/
	switch_event_node_t *node;
} globals;

/*状态迁移表*/
typedef struct 
{
   uint32_t event; /*触发事件 */                            
   uint32_t CurState;   /*当前状态*/
   void (*eventActFun)(void *pdata); /*动作处理函数*/
   int32_t NextState;     /*跳转的状态*/
}FsmTable_T;
//状态
typedef struct FSM_s
{
    FsmTable_T *FsmTable;         /* 状态迁移表 */
    uint32_t curState;             /* 状态机当前状态 */
    uint32_t stuMaxNum;            /* 状态机状态迁移数量 */
}FSM_T;

/*排队处理的数据结构,直接从session pool中申请内存，当session结束直接释放.*/
typedef struct{
	int32_t running;
    char session_uuid[SWITCH_UUID_FORMATTED_LENGTH + 1];
	char ir_req_uuid[SWITCH_UUID_FORMATTED_LENGTH + 1];
	char agent_id[MAX_USER_NUM + 1];
	char userId[MAX_USER_NUM + 1]; /*主叫号码*/
	char callerId[MAX_USER_NUM + 1];/*被叫号码*/
	char skillId[SWITCH_UUID_FORMATTED_LENGTH];
	uint32_t session_begin_time; /*记录排队的开始时间*/
    uint32_t ir_req_time;        /*最近发送ir::req时间*/
	switch_core_session_t *cq_session; /*把session的指针存下来*/
	switch_channel_t *channel;         /*把通道的指针存下来*/
	switch_queue_t *event_queue;       /*线程之间通信的消息队列*/
	queue_status_t queue_status;
	int32_t current_counter_time;     /*当前排队的次数...*/
	FSM_T FsmContext;                 /*状态机处理*/
}queue_deal_pcb_t;

/*函数声明区*/
void callqueue_build_ircancel_and_fire(queue_deal_pcb_t *phandler);
void callqueue_build_ccreq_and_fire(queue_deal_pcb_t *phandler);
void callqueue_build_cccancel_and_fire(queue_deal_pcb_t *phandler);
void callqueue_build_irreq_and_fire(queue_deal_pcb_t *phandler,int type);


/*当前时间，单位为秒*/
switch_time_t local_epoch_time_now(switch_time_t *t)
{
	switch_time_t now = switch_micro_time_now() / 1000000; /* APR_USEC_PER_SEC */
	if (t) {
		*t = now;
	}
	return now;
}

/*事件处理函数定义*/
/*排队成功处理*/
static void ActFun_QueueSuccessHandle(void *parm)
{
    queue_deal_pcb_t *ph = (queue_deal_pcb_t *)parm;
	switch_core_session_t *session = ph->cq_session;
	switch_channel_t *channel = ph->channel;
	char *play_voice = NULL;
    
	switch_assert(session != NULL);
	switch_assert(channel != NULL);

    play_voice = globals.success_voice;

	//播放提示音
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "play voice file name:%s\n",play_voice);
    switch_ivr_stop_displace_session(session, play_voice);
	switch_ivr_displace_session(session, play_voice, 0, NULL);
	return;
}
/*队列满处理*/
static void ActFun_QueueFullHandle(void *parm)   
{
    queue_deal_pcb_t *ph = (queue_deal_pcb_t *)parm;
	switch_core_session_t *session = ph->cq_session;
	switch_channel_t *channel = ph->channel;
	char *play_voice = NULL;
    
	switch_assert(session != NULL);
	switch_assert(channel != NULL);

    play_voice = globals.full_voice;

	/*播放提示音*/
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "play voice file name:%s\n",play_voice);
    switch_ivr_stop_displace_session(session, play_voice);
	switch_ivr_displace_session(session, play_voice, 0, NULL);
	return;
}
 /*队列忙处理*/
static void ActFun_QueueBusyHandle(void *parm)  
{
	queue_deal_pcb_t *ph = (queue_deal_pcb_t *)parm;
	switch_core_session_t *session = ph->cq_session;
	switch_channel_t *channel = ph->channel;
	char *play_voice = NULL;
    
	switch_assert(session != NULL);
	switch_assert(channel != NULL);

  
    play_voice = globals.queue_busy_voice;
    
	/*播放提示音*/
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "play voice file name:%s\n",play_voice);
    switch_ivr_stop_displace_session(session, play_voice);
	switch_ivr_displace_session(session, play_voice, 0, NULL);
	return;
}
 /*用户主动取消处理*/
static void ActFun_QueueDtmfCancelHandle(void *parm)  
{
	queue_deal_pcb_t *ph = (queue_deal_pcb_t *)parm;
	switch_core_session_t *session = ph->cq_session;
	switch_channel_t *channel = ph->channel;
	char *play_voice = NULL;
    
	switch_assert(session != NULL);
	switch_assert(channel != NULL);

    play_voice = globals.cancel_voice;

	/*播放提示音*/
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "play voice file name:%s\n",play_voice);
    switch_ivr_stop_displace_session(session, play_voice);
	switch_ivr_displace_session(session, play_voice, 0, NULL);
	/*给ir 和cc发送取消事件*/
    callqueue_build_ircancel_and_fire(ph);
    callqueue_build_cccancel_and_fire(ph);
	return;
}
/*队列错误处理*/
static void ActFun_QueueErrorHandle(void *parm)  
{
	queue_deal_pcb_t *ph = (queue_deal_pcb_t *)parm;
	switch_core_session_t *session = ph->cq_session;
	switch_channel_t *channel = ph->channel;
	char *play_voice = NULL;
    
	switch_assert(session != NULL);
	switch_assert(channel != NULL);

    play_voice = globals.error_voice;
	/*播放提示音*/
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "play voice file name:%s\n",play_voice);
    switch_ivr_stop_displace_session(session, play_voice);
	switch_ivr_displace_session(session, play_voice, 0, NULL);
	/**/
	return;
}
/*分配坐席成功处理*/
static void ActFun_QueueAgentSuccHandle(void *parm)   
{
    queue_deal_pcb_t *ph = (queue_deal_pcb_t *)parm;
	switch_core_session_t *session = ph->cq_session;
	switch_channel_t *channel = ph->channel;
	char *play_voice = NULL;
    
	switch_assert(session != NULL);
	switch_assert(channel != NULL);
    /* 播放提示音阶段*/

    play_voice = globals.agent_succ_voice;

	/*播放提示音*/
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "play voice file name:%s\n",play_voice);
    switch_ivr_stop_displace_session(session, play_voice);
	switch_ivr_displace_session(session, play_voice, 0, NULL);

	/*给CC发送cc::req事件*/
    callqueue_build_ccreq_and_fire(ph);
	return;
}
/*没有坐席签入处理*/
static void ActFun_QueueNoAgenInHandle(void *parm) 
{
	queue_deal_pcb_t *ph = (queue_deal_pcb_t *)parm;
	switch_core_session_t *session = ph->cq_session;
	switch_channel_t *channel = ph->channel;
	char *play_voice = NULL;
    
	switch_assert(session != NULL);
	switch_assert(channel != NULL);


     play_voice = globals.no_angent_voice;

	/*播放提示音*/
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "play voice file name:%s\n",play_voice);
    switch_ivr_stop_displace_session(session, play_voice);
	switch_ivr_displace_session(session, play_voice, 0, NULL);
	return;
}
/*超时处理*/ 
static void ActFun_QueueTimeOutHandle(void *parm)
{
	queue_deal_pcb_t *ph = (queue_deal_pcb_t *)parm;
	switch_core_session_t *session = ph->cq_session;
	switch_channel_t *channel = ph->channel;
	char *play_voice = NULL;
    //switch_time_t now_time = local_epoch_time_now(NULL);
    
	switch_assert(session != NULL);
	switch_assert(channel != NULL);
    

    play_voice = globals.queue_session_timeout_voice;

	//播放提示音
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "play voice file name:%s\n",play_voice);
    switch_ivr_stop_displace_session(session, play_voice);
	switch_ivr_displace_session(session, play_voice, 0, NULL);

	return;
}

/*坐席忙处理*/ 
static void ActFun_QueueAgentBusyHandle(void *parm)
{
	queue_deal_pcb_t *ph = (queue_deal_pcb_t *)parm;
	switch_core_session_t *session = ph->cq_session;
	switch_channel_t *channel = ph->channel;
	char *play_voice = NULL;
    
	switch_assert(session != NULL);
	switch_assert(channel != NULL);

    play_voice = globals.agent_busy_voice;/*坐席忙语音*/

	//播放提示音
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "play voice file name:%s\n",play_voice);
    switch_ivr_stop_displace_session(session, play_voice);
	switch_ivr_displace_session(session, play_voice, 0, NULL);
	//给IR发ir::cancel消息
    callqueue_build_ircancel_and_fire(ph);
	return;
}
/*超时重新发送ir::req*/
void ActionFun_ResendIRreqEvent(void *parm)
{
	queue_deal_pcb_t *ph = (queue_deal_pcb_t *)parm;
	switch_core_session_t *session = ph->cq_session;
	switch_channel_t *channel = ph->channel;
	//char *play_voice = NULL;
	//switch_time_t now_time = local_epoch_time_now(NULL);

    switch_assert(session != NULL);
	switch_assert(channel != NULL);
	/*再次发送ir::req消息，这里不给提示*/
    ph->current_counter_time++;
	ph->ir_req_time = time(NULL);/*单次请求的开始时间*/

    /*重新发送ir::req*/
    callqueue_build_irreq_and_fire(ph,1);
	return ;
}
/*事件处理函数结束*/

/*事件处理表*/
static FsmTable_T g_stFsmTable[] = 
{
	/*触发事件  ,      当前状态  ,           函数动作   ,           处理后的状态*/
	{ IR_RES_SUCCESS , QUEUE_IR_REQ_S , ActFun_QueueSuccessHandle , QUEUE_IR_SUCCESS_S },
	{ IR_RES_FULL,QUEUE_IR_REQ_S , ActFun_QueueFullHandle , QUEUE_DISTRIBUTE_ERROR_S },
	{ IR_RES_QUEUE_BUSY,QUEUE_IR_REQ_S , ActFun_QueueBusyHandle , QUEUE_DISTRIBUTE_ERROR_S },
	{ DTMF_CANCEL_REQ,QUEUE_IR_REQ_S , ActFun_QueueDtmfCancelHandle , QUEUE_CC_CANCEL_S },
	{ IR_RES_QUEUE_ERROR, QUEUE_IR_REQ_S,ActFun_QueueErrorHandle,QUEUE_DISTRIBUTE_ERROR_S},
	{ IR_RES_NO_AGENT_IN, QUEUE_IR_REQ_S,ActFun_QueueNoAgenInHandle, QUEUE_DISTRIBUTE_ERROR_S },
	{ ONE_SESSION_TIMEOUT, QUEUE_IR_REQ_S,ActFun_QueueTimeOutHandle,QUEUE_TIMEOUT_S },
	{ ONE_REQ_TIMEOUT, QUEUE_IR_REQ_S,ActFun_QueueTimeOutHandle,QUEUE_INIT_S },

	{ IR_RES_AGENT_DISTRIBUTE ,QUEUE_IR_SUCCESS_S,ActFun_QueueAgentSuccHandle,QUEUE_CC_REQ_S },
	{ IR_RES_QUEUE_BUSY, QUEUE_IR_SUCCESS_S,ActFun_QueueBusyHandle,QUEUE_DISTRIBUTE_ERROR_S},
	{ DTMF_CANCEL_REQ, QUEUE_IR_SUCCESS_S,ActFun_QueueDtmfCancelHandle,QUEUE_CC_CANCEL_S},
	{ IR_RES_NO_AGENT_IN, QUEUE_IR_SUCCESS_S,ActFun_QueueNoAgenInHandle,QUEUE_DISTRIBUTE_ERROR_S},
	{ IR_RES_FULL,QUEUE_IR_SUCCESS_S,ActFun_QueueFullHandle,QUEUE_DISTRIBUTE_ERROR_S},
	{ IR_RES_QUEUE_ERROR,QUEUE_IR_SUCCESS_S,ActFun_QueueErrorHandle,QUEUE_DISTRIBUTE_ERROR_S},
	{ ONE_SESSION_TIMEOUT, QUEUE_IR_SUCCESS_S,ActFun_QueueTimeOutHandle,QUEUE_TIMEOUT_S },
	{ ONE_REQ_TIMEOUT, QUEUE_IR_SUCCESS_S,ActFun_QueueTimeOutHandle,QUEUE_INIT_S },

	{ ONE_SESSION_TIMEOUT, QUEUE_CC_REQ_S,ActFun_QueueTimeOutHandle,QUEUE_TIMEOUT_S },
	{ ONE_REQ_TIMEOUT, QUEUE_CC_REQ_S,ActFun_QueueTimeOutHandle,QUEUE_INIT_S },
	{ DTMF_CANCEL_REQ,QUEUE_CC_REQ_S,ActFun_QueueDtmfCancelHandle,QUEUE_CC_CANCEL_S },
	{ CC_RES_AGENT_BUSY,QUEUE_CC_REQ_S,ActFun_QueueAgentBusyHandle,QUEUE_INIT_S }, /*坐席忙*/

	{ ONE_SESSION_TIMEOUT,QUEUE_INIT_S,ActFun_QueueTimeOutHandle,QUEUE_TIMEOUT_S},
	{ ONE_REQ_TIMEOUT,QUEUE_INIT_S,ActionFun_ResendIRreqEvent,QUEUE_IR_REQ_S},
	{ DTMF_CANCEL_REQ,QUEUE_INIT_S,ActFun_QueueDtmfCancelHandle, QUEUE_CC_CANCEL_S},
};

/*定义状态机表的大小*/
#define FSM_TABLE_MAX_NUM sizeof(g_stFsmTable)/sizeof(g_stFsmTable[0])

//加载配置文件中的放音文件以及相关参数...
static switch_status_t load_config(void)
{
    switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_xml_t cfg, xml, settings,param;
	char sounds_dir[128] = {0}; /*声音文件默认目录*/
	char file_dir[256] = {0};/*声音文件全目录*/
	char *psdir = NULL;
    
	if (!(xml = switch_xml_open_cfg(global_cf, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open of %s failed\n", global_cf);
		status = SWITCH_STATUS_TERM;
		goto end;
	}

    switch_mutex_lock(globals.mutex);
	if ((settings = switch_xml_child(cfg, "settings"))) {
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			char *var = (char *) switch_xml_attr_soft(param, "name");
			char *val = (char *) switch_xml_attr_soft(param, "value");

			if (!strcasecmp(var, "init-voice")) {/*初始化背景音乐*/
				globals.init_voice = strdup(val);
			} else if (!strcasecmp(var, "success-voice")) {/*排队成功提示*/
				globals.success_voice = strdup(val);
			} else if (!strcasecmp(var, "full-voice")) {/*队列满提示*/
				globals.full_voice = strdup(val);
			} else if (!strcasecmp(var,"queue-busy-voice")){/*队列忙提示*/
                globals.queue_busy_voice = strdup(val);
			}else if (!strcasecmp(var,"agent-busy-voice")){/*坐席忙提示*/
                globals.agent_busy_voice = strdup(val);
			}else if (!strcasecmp(var,"agent-succ-voice")){/*分配坐席成功提示*/
                globals.agent_succ_voice = strdup(val);
			} else if (!strcasecmp(var, "cancel-voice")) {/*用户取消排队提示*/
				globals.cancel_voice = strdup(val);
			} else if (!strcasecmp(var, "error-voice")) { /*发生错误提示*/
				globals.error_voice = strdup(val);
			} else if (!strcasecmp(var, "no-angent-voice")) {/*没有坐席签入提示*/
				globals.no_angent_voice = strdup(val);
			} else if (!strcasecmp(var, "wait-number-voice")) {/*等待的数量提示*/
				globals.wait_number_voice = strdup(val);
			} else if (!strcasecmp(var, "user-break-voice")){/**/
                globals.user_break_voice = strdup(val);
			} else if (!strcasecmp(var, "max-queue-times")){/*最大排队次数*/
                globals.max_queue_times = atoi(val);
			}else if (!strcasecmp(var, "max-queue-session-time")){/*排队最长时间*/
                globals.max_queue_session_time = atol(val);
			}else if(!strcasecmp(var, "max-one-queue-wait-time")){/*单次等待最长时间*/
				globals.max_one_queue_wait_time = atol(val);
			}
		}
	}
	/*获取声音目录*/
	psdir = switch_core_get_variable("sounds_dir");
	if (psdir)
	{
		switch_snprintf(sounds_dir,128,"%s/zh/cn/sinmei/queue/",psdir);
	}
	else
	{
		strncpy(sounds_dir,"/usr/local/freeswitch/sounds/zh/cn/sinmei/queue/",128);
	}
	/*如果配置文件中没有配置，则采用默认配置*/
	if (zstr(globals.init_voice))
	{
       globals.init_voice = strdup("local_stream://moh");
	}
	if (!globals.max_queue_times)
	{
		globals.max_queue_times = 3;/*默认三次*/
	}
	if (!globals.max_queue_session_time)
	{
		globals.max_queue_session_time = 120; /*默认120秒*/
	}
	if (!globals.max_one_queue_wait_time)
	{
		globals.max_one_queue_wait_time = 30;/*单次默认30秒*/
	}
	/*默认值设置*/
    if (!globals.success_voice)
    {
		memset(file_dir,0,256);
		switch_snprintf(file_dir,256,"%s/success-voice.wav",sounds_dir);
		globals.success_voice = strdup(file_dir);
    }
	if (!globals.full_voice)
    {
		memset(file_dir,0,256);
		switch_snprintf(file_dir,256,"%s/full-voice.wav",sounds_dir);
		globals.full_voice = strdup(file_dir);
    }
	if (!globals.queue_busy_voice)
    {
		memset(file_dir,0,256);
		switch_snprintf(file_dir,256,"%s/queue-busy-voice.wav",sounds_dir);
		globals.queue_busy_voice = strdup(file_dir);
    }
	if (!globals.agent_busy_voice)
    {
		memset(file_dir,0,256);
		switch_snprintf(file_dir,256,"%s/agent-busy-voice.wav",sounds_dir);
		globals.agent_busy_voice = strdup(file_dir);
    }
	if (!globals.agent_succ_voice)
    {
		memset(file_dir,0,256);
		switch_snprintf(file_dir,256,"%s/agent-succ-voice.wav",sounds_dir);
		globals.agent_succ_voice=strdup(file_dir);
    }
	if (!globals.cancel_voice)
    {
		memset(file_dir,0,256);
		switch_snprintf(file_dir,256,"%s/cancel-voice.wav",sounds_dir);
		globals.cancel_voice=strdup(file_dir);
    }
	if (!globals.error_voice)
    {
		memset(file_dir,0,256);
		switch_snprintf(file_dir,256,"%s/error-voice.wav",sounds_dir);
		globals.error_voice=strdup(file_dir);
    }
	if (!globals.no_angent_voice)
    {
		memset(file_dir,0,256);
		switch_snprintf(file_dir,256,"%s/no_angent-voice.wav",sounds_dir);
		globals.no_angent_voice=strdup(file_dir);
    }
	if (!globals.queue_session_timeout_voice)
    {
		memset(file_dir,0,256);
		switch_snprintf(file_dir,256,"%s/queue-session-timeout-voice.wav",sounds_dir);
		globals.queue_session_timeout_voice=strdup(file_dir);
    }
	if (!globals.wait_number_voice)
    {
		memset(file_dir,0,256);
		switch_snprintf(file_dir,256,"%s/wait-number-voice.wav",sounds_dir);
		globals.wait_number_voice=strdup(file_dir);
    }
	if (!globals.user_break_voice)
    {
		memset(file_dir,0,256);
		switch_snprintf(file_dir,256,"%s/user-break-voice.wav",sounds_dir);
		globals.user_break_voice=strdup(file_dir);
    }
	switch_mutex_unlock(globals.mutex);
 end:
	 if (xml) {
		switch_xml_free(xml);
	}
	return status;
}

/**********************************************************************
*订阅消息：
*         这里订阅所有消息，但是处理时，只处理两种消息 CUSTOM和DTMF
*         对于CUSTOM消息，只处理cc::resp和ir::resp
***********************************************************************/
static void event_handler(switch_event_t *event)
{
	switch_event_t *clone = NULL; //克隆一个事件
	switch_status_t qstatus = SWITCH_STATUS_FALSE;
	int clone_flag = 0;
	char *uuid = NULL;
	char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1]={0};
	switch_queue_t *event_queue = NULL;

	switch_assert(event != NULL);

	//switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "get msg id:%d,subname:%s\n",event->event_id,event->subclass_name);

	 switch (event->event_id) {
		case SWITCH_EVENT_DTMF:
			clone_flag = 1;
		    uuid = switch_str_nil(switch_event_get_header(event, "Caller-Unique-ID")); //dtmf，取uuid
		    break;
		case SWITCH_EVENT_CUSTOM:
			if ((!strcasecmp(event->subclass_name,IR_EVENT_RES))||
			     (!strcasecmp(event->subclass_name,CC_EVENT_RES))||
			      (!strcasecmp(event->subclass_name,CALLQUEUE_TIMEOUT)))
			{
				clone_flag = 1;	
				uuid = switch_str_nil(switch_event_get_header(event, "unique_id")); //ir::resp,cc::resp取uuid
			}
			break;
		default:
			clone_flag = 0;
		    break;
	  }
  /*克隆DTMF和CUSTOM的 IR::resp CC::resp两个消息给 callqueue app处理*/
  if (clone_flag)
  {
	  /*dump event */
	 //DUMP_EVENT(event); 
	/*获取消息中的unique-id*/
    if (uuid)
    {
		strncpy(uuid_str,uuid,SWITCH_UUID_FORMATTED_LENGTH);
    }
	else
	 {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "get uuid from session error!\n");
		return; //direct to return
	 }
	/*获取hash的handler处理句柄...*/
    if (!(event_queue = switch_core_hash_find_locked(globals.queue_hash,uuid_str,globals.read_mutex)))
    {
		 switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "get phandler pointer hash error!\n");
		 return ;
    }
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "key:%s,get msg id:%d,subname:%s,queue id addr:0x%lu\n",
		                                                  uuid_str,
		                                                  event->event_id,
		                                                  event->subclass_name,
		                                                    (long)event_queue);
	/*发送消息到对应的queue*/
    if (switch_event_dup(&clone, event) == SWITCH_STATUS_SUCCESS) {
	   qstatus = switch_queue_trypush(event_queue, clone); /*压到队列中...*/
	   if (qstatus == SWITCH_STATUS_SUCCESS) {
		   switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "push msg to event_queue!\n");
	    } else {
			switch_event_destroy(&clone);
		}
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Memory Error,clone event error!\n");
	}
  }
}
/********************************************************************
*input para:
*          phandler:模块处理上下文
*output para:
*          void
*说明：构造向acd请求路由消息ir::req
*********************************************************************/
void callqueue_build_ircancel_and_fire(queue_deal_pcb_t *phandler)
{
	switch_event_t *event = NULL;
	switch_core_session_t *session = phandler->cq_session;

	switch_channel_t *channel = phandler->channel;

	if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM,IR_EVENT_REQ) == SWITCH_STATUS_SUCCESS) {
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "unique_id", phandler->session_uuid);
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "access_type", "1");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "request_type", "3");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "event_src", "queueHangupHook");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "caller_number", phandler->userId);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "callee_number", "");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "ai_all_agent", phandler->ir_req_uuid);
		switch_event_fire(&event);
	}
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "callqueue_build_ircancel_and_fire\n");
	return;
}

/********************************************************************
*input para:
*          phandler:模块处理上下文
*output para:
*          void
*说明：构造向acd请求路由消息cc::cancel
*********************************************************************/
void callqueue_build_ccreq_and_fire(queue_deal_pcb_t *phandler)
{
	switch_event_t *event = NULL;
	switch_core_session_t *session = phandler->cq_session;

	switch_channel_t *channel = phandler->channel;

	if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM,CC_EVENT_REQ) == SWITCH_STATUS_SUCCESS) {
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "unique_id", phandler->session_uuid);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "target_agent", phandler->agent_id);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "count", "0");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "dataSource", switch_str_nil(switch_channel_get_variable(channel, "dataSource")));
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "callDirect", switch_str_nil(switch_channel_get_variable(channel, "callDirect")));
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "callType", switch_str_nil(switch_channel_get_variable(channel, "callType")));
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "batchId", switch_str_nil(switch_channel_get_variable(channel, "batchId")));
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "skillId", phandler->skillId);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "callNum", switch_str_nil(switch_channel_get_variable(channel, "sip_to_user")));
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "callee_number", switch_str_nil(switch_channel_get_variable(channel, "sip_to_user")));
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "ai_all_agent", switch_str_nil(switch_channel_get_variable(channel, "ai_all_agent")));
		switch_event_fire(&event);
	}
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "callqueue_build_ccreq_and_fire\n");
	return;
}

/********************************************************************
*input para:
*          phandler:模块处理上下文
*output para:
*          void
*说明：构造向acd请求路由消息cc::cancel
*********************************************************************/
void callqueue_build_cccancel_and_fire(queue_deal_pcb_t *phandler)
{
	switch_event_t *event = NULL;
	switch_core_session_t *session = phandler->cq_session;

	switch_channel_t *channel = phandler->channel;

	if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM,CC_EVENT_REQ) == SWITCH_STATUS_SUCCESS) {
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "unique_id", phandler->session_uuid);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "target_agent", phandler->agent_id);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "count", "0");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "action", "cancel");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "dataSource", switch_str_nil(switch_channel_get_variable(channel, "dataSource")));
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "callDirect", switch_str_nil(switch_channel_get_variable(channel, "callDirect")));
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "callType", switch_str_nil(switch_channel_get_variable(channel, "callType")));
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "batchId", switch_str_nil(switch_channel_get_variable(channel, "batchId")));
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "skillId", phandler->skillId);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "callNum", switch_str_nil(switch_channel_get_variable(channel, "sip_to_user")));
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "callee_number", switch_str_nil(switch_channel_get_variable(channel, "sip_to_user")));
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "ai_all_agent", switch_str_nil(switch_channel_get_variable(channel, "ai_all_agent")));
		switch_event_fire(&event);
	}
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "callqueue_build_cccancel_and_fire\n");
	return;
}

/********************************************************************
*input para:
*          phandler:模块处理上下文
*output para:
*          void
*说明：构造向acd请求路由消息ir::req
*********************************************************************/
void callqueue_build_irreq_and_fire(queue_deal_pcb_t *phandler,int type)
{
	switch_event_t *event = NULL;

	switch_uuid_t uuid; /*重新请求的ir_uuid*/
	switch_core_session_t *session = phandler->cq_session;
	uint32_t now_time = time(NULL);
	char buf[128] = {0};

	switch_channel_t *channel = phandler->channel;
	switch_uuid_get(&uuid);
	switch_uuid_format(phandler->ir_req_uuid, &uuid); /*重新分配新的ir uuid*/
	switch_snprintf(buf,128,"%d",now_time);
	phandler->ir_req_time = now_time;

	if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM,IR_EVENT_REQ) == SWITCH_STATUS_SUCCESS) {
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "unique_id", phandler->session_uuid);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "session_id",phandler->ir_req_uuid);/*每次请求给出新的uuid*/
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "queue_id","0");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "callee_number",phandler->userId);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "caller_number",phandler->callerId);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "time_tok",buf);
		if (4 == type)
		{
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "request_type","4");/*初次申请*/
		}
		else if (1 == type)
		{
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "request_type","1");/*初次申请*/
		}
		else
		{
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "callqueue_build_irreq_and_fire type is unknown\n");
			return;
		}
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "access_type","1");/*这里暂时填1*/
		/*skillId从通道变量中获取*/
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "skillId",switch_str_nil(switch_channel_get_variable(channel, "skillId")));
		switch_event_fire(&event);
	}
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "callqueue_build_irreq_and_fire\n");
	return;
}

/***************************************************************************************
*input para:
*           event handler,phandler
*从事件中识别对应的消息类型，同时把事件中相关处理参数保存到处理块中
****************************************************************************************/
uint32_t callqueue_get_resptype_from_event(switch_event_t *pevent,queue_deal_pcb_t *phandler)
{
	switch_event_t *event = pevent;
	int response_type;
	char digit_value[16]={0};
	int status = -1;

	switch(event->event_id)
	{
		case SWITCH_EVENT_CUSTOM:
			if (!strcasecmp(event->subclass_name,IR_EVENT_RES)) /*IR::RESP消息*/
			{
				response_type = atoi(switch_event_get_header(event, "response_type"));
				status =  response_type;
				/*分配到坐席，保存已经分配的坐席id到pcb中*/
				if (IR_RES_AGENT_DISTRIBUTE == response_type)
				{
                   strncpy(phandler->agent_id,switch_str_nil(switch_event_get_header(event, "target_agent")),MAX_USER_NUM);
				   strncpy(phandler->skillId,switch_str_nil(switch_event_get_header(event, "target_skillId")),MAX_USER_NUM);
				}
			}
			if (!strcasecmp(event->subclass_name,CC_EVENT_RES)) /*CC::RESP消息*/
			{
                 status =  CC_RES_AGENT_BUSY;
			}
			if (!strcasecmp(event->subclass_name,CALLQUEUE_TIMEOUT)) /*超时事件*/
			{
				 /*整个排队超时*/
                 if (!strcasecmp(switch_event_get_header(event, "CQ-Action"),SESSION_TIMEOUT_EVENT))
                 {
                    status = ONE_SESSION_TIMEOUT;
                 }
				 /*单次排队超时*/
                 if (!strcasecmp(switch_event_get_header(event, "CQ-Action"),ONE_REQ_TIMEOUT_EVENT))
                 {
                     status = ONE_REQ_TIMEOUT;
                 }
			}
			break;
		/*DTMF信令，'*'取消，'#'继续请求*/
		case SWITCH_EVENT_DTMF:
		    strcpy(digit_value,switch_event_get_header(event, "DTMF-Digit"));
		    if (!strcasecmp(digit_value,"*"))
		    {
				status =  DTMF_CANCEL_REQ;
		    }
			else if (!strcasecmp(digit_value,"#"))
			{
                status = DTMF_CONTINUE_REQ;
			}

			break;
		default:
			status = -1;
		break;
	}
	return status;
}
/******************************************************************
* Function  : FSM_StateTransfer
* Description : 状态转换
* Input Para  : 
* Output Para : 
* Return Value: 
******************************************************************/
static void FSM_StateTransfer(FSM_T *pFsm, uint32_t state)
{
    switch_assert(pFsm != NULL);
    pFsm->curState = state;
}

/*******************************************************************
* Function  : FSM_EventHandle
* Description : 状态机处理函数
* Input Para  : pFsm状态机对象, event触发事件, parm动作执行参数
* Output Para : 
* Return Value: 
*******************************************************************/
void FSM_EventHandle(FSM_T *pFsm, uint32_t event, void *parm)
{
    FsmTable_T *pAcTable = NULL;
    void (*eventActFun)(void *) = NULL;
    uint32_t NextState;
    uint32_t CurState = 0;
    uint32_t flag = 0;

    switch_assert(pFsm != NULL);
    CurState = pFsm->curState;
	pAcTable = pFsm->FsmTable;
      
    for (uint8_t i = 0; i < pFsm->stuMaxNum; i++)/* 遍历状态表*/
    {
        if (event == pAcTable[i].event && CurState == pAcTable[i].CurState)
        {
            flag = 1;
            eventActFun = pAcTable[i].eventActFun;
            NextState = pAcTable[i].NextState;
            break;
        }
    }
    if (flag)
    {
        if (eventActFun != NULL)
        {
            eventActFun(parm);  /* 执行相应动作*/
        }
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "trans status: current status:%d,next status:%d\n",CurState,NextState);
        FSM_StateTransfer(pFsm, NextState); /* 状态转换*/
    }
    else
    {
        // do nothing
    }
}
 
/*==================================================================
* Function  : FSM_Init
* Description : 状态机初始化
* Input Para  : pFsm状态机对象，pTable状态迁移表，stuMaxNum迁移表数量
*               curState当前状态
* Output Para : 
* Return Value: 
==================================================================*/
void FSM_Init(FSM_T *pFsm, FsmTable_T *pTable, uint32_t stuMaxNum, uint32_t curState)
{
    switch_assert(pFsm != NULL);
    pFsm->FsmTable = pTable;
    pFsm->curState = curState;
    pFsm->stuMaxNum = stuMaxNum;
}

/***************************************************************************
*处理有两部分功能：
*           检查一次请求是否超时，以及整个排队是否超时。
*           对于处于IR_Q_ERROR和Q_cancel状态的排队进行退出处理
****************************************************************************/
switch_bool_t callqueue_check_timeout_event(queue_deal_pcb_t *phandler)
{
    queue_deal_pcb_t *ph = phandler;
    uint32_t queue_times = ph->current_counter_time;
	uint32_t module_current_status = ph->FsmContext.curState; /*获取当前模块的状态*/
    uint32_t now_time = time(NULL); /*获取当前时间*/
	uint32_t session_begin_time = ph->session_begin_time;
	uint32_t one_req_time = ph->ir_req_time;
	switch_core_session_t *session = ph->cq_session;
	switch_channel_t *channel = ph->channel;
	switch_event_t *event = NULL;

    
	/*处于用户取消或分配失败状态下，直接退出排队*/
	if (((module_current_status == QUEUE_CC_CANCEL_S)||(QUEUE_DISTRIBUTE_ERROR_S == module_current_status))&&(now_time - one_req_time > globals.max_one_queue_wait_time))
	{
		/*give a log mark here*/
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "callqueue_check_timeout_event %d \n",module_current_status);
        return SWITCH_TRUE;
	}
	/*如果排队的对话超时或单次请求超时同时超过请求次数，发一个session超时event*/
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "max_queue_session_time:%d,now_time:%d, req_time:%d\n",(uint32_t)globals.max_queue_session_time,(uint32_t)now_time,(uint32_t)session_begin_time);
	if ((globals.max_queue_session_time < (now_time - session_begin_time))||
		    ((queue_times > globals.max_queue_times) && (module_current_status == QUEUE_INIT_S)))
	{
        if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM,CALLQUEUE_TIMEOUT) == SWITCH_STATUS_SUCCESS) {
			switch_channel_event_set_data(channel, event);
			switch_event_add_header(event, SWITCH_STACK_BOTTOM, "CQ-Action", SESSION_TIMEOUT_EVENT);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "unique_id", ph->session_uuid);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "CQ-IRREQ-UUID",ph->ir_req_uuid);
		    switch_event_fire(&event);
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "callqueue_check_timeout_event send event: SESSION_TIMEOUT_EVENT\n");
			return SWITCH_TRUE;
	    }
		
	}
   /*单次请求超时，未达到请求次数，发送一个单次请求超时事件*/
   switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(now_time-one_req_time):%d, globals.max_one_queue_wait_time:%d,queue_times:%d,globals.max_queue_times:%d\n",
	             (uint32_t)(now_time - one_req_time),(uint32_t)globals.max_one_queue_wait_time,queue_times,(uint32_t)globals.max_queue_times);
    if (((now_time - one_req_time) > globals.max_one_queue_wait_time)&&(queue_times < globals.max_queue_times))
    {
       if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM,CALLQUEUE_TIMEOUT) == SWITCH_STATUS_SUCCESS) {
			switch_channel_event_set_data(channel, event);
			switch_event_add_header(event, SWITCH_STACK_BOTTOM, "CQ-Action", ONE_REQ_TIMEOUT_EVENT);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "unique_id", ph->session_uuid);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "CQ-IRREQ-UUID",ph->ir_req_uuid);
		    switch_event_fire(&event);
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "callqueue_check_timeout_event send event: ONE_REQ_TIMEOUT_EVENT\n");
			return SWITCH_TRUE;
	    }
    }
	return SWITCH_FALSE;
}

/****************************************************************************
*  处理acd相关消息的线程，主线程用户放音，该线程用户处理消息
*
*****************************************************************************/
void *SWITCH_THREAD_FUNC callqueue_thread_run(switch_thread_t *thread, void *obj)
{
    queue_deal_pcb_t *m = (queue_deal_pcb_t *) obj;

	switch_channel_t *channel = m->channel;
	void *pop = NULL;

	switch_core_session_t *session = m->cq_session;

       
     switch_assert(session != NULL);
	 switch_assert(channel != NULL);
	/*初始化状态机*/
    FSM_Init(&m->FsmContext, g_stFsmTable, FSM_TABLE_MAX_NUM, QUEUE_IR_REQ_S);
    /*首先发送排队请求消息*/
    callqueue_build_irreq_and_fire(m,1);
	while(switch_channel_ready(channel) && globals.running && m->running)
	{
		/*从队列中取消息*/
		if (switch_queue_trypop(m->event_queue, &pop) == SWITCH_STATUS_SUCCESS) {
			switch_event_t *dnode = (switch_event_t *) pop;
			uint32_t resp_type = callqueue_get_resptype_from_event(dnode,m);
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "get msg type:%d\n",resp_type);
			FSM_EventHandle(&m->FsmContext, resp_type, (void *)m);
            switch_event_destroy(&dnode);
		}
		else
		{	
			switch_yield(1000000);/*每5秒扫一次*/
		}
		/*检查超时,发送超时事件*/
        if (callqueue_check_timeout_event(m) == SWITCH_TRUE)
        { 
			/*should log mark here*/
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "exit queue by timeout or error or cancel\n");
			break;
        }
		/*检查通道是否处于桥接状态，如果已经桥接，则退出线程处理...*/
		if (switch_channel_test_flag(channel, CF_BRIDGED)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "call already transfer to agent\n");
			break;
		}
	}
	/*设置退出通道变量*/
    switch_channel_set_variable(channel, "queue_agent_found", "found");
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "callqueue_thread_run thread exit \n");
	return NULL;
}

/************************************************
*
*销毁前，如果队列中有消息，删除对应的消息。
*
*************************************************/
static void callqueue_flush_event_queue(switch_queue_t *event_queue)
{
	void *pop = NULL;

	if (event_queue) {
		while (switch_queue_trypop(event_queue, &pop) == SWITCH_STATUS_SUCCESS) {
		    switch_event_t *pevent = (switch_event_t *) pop;
			if (!pop)
				continue;
			switch_event_destroy(&pevent);
		}
	}
	return;
}

/**************************************************
  dialplan  app
  descripts:
            send ir::req to acd
            recv ir::resp from acd
			send cc::req to cc
			recv cc::resp from cc
			recv dtmf event
***************************************************/
SWITCH_STANDARD_APP(queue_function)
{
	switch_channel_t *channel = NULL;
	
	queue_deal_pcb_t *h = NULL;/*处理句柄...*/
	char *uuid = NULL;
	char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1] ={0};
	switch_thread_t *thread;
	switch_threadattr_t *thd_attr = NULL;
	const char *p;
	switch_bool_t agent_found = SWITCH_FALSE;
 
    channel = switch_core_session_get_channel(session);

   	if (!(h = switch_core_session_alloc(session, sizeof(*h)))) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Memory Error\n");
		return;
	}

	/*从session中获取uuid字符串*/
    uuid = switch_core_session_get_uuid(session);
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "get uuid:%s\n",uuid);
	if (uuid)
	{
		strncpy(uuid_str,uuid,SWITCH_UUID_FORMATTED_LENGTH + 1);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "uuid ok: uuid is %s \n",uuid_str);
	}
	else
	{
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "callqueue app uuid Error: uuid is NULL \n");
		goto end;
	}
	/*消息队列初始化及相关初始化...*/
	switch_queue_create(&h->event_queue, MAX_QUEUE_LEN, switch_core_session_get_pool(session));//队列从core pool中分配内存...

	strncpy(h->session_uuid,uuid,SWITCH_UUID_FORMATTED_LENGTH);
	h->running = 1;
	h->queue_status = QUEUE_INIT_S;//状态机置为初始化状态...
	h->session_begin_time = time(NULL);
	h->cq_session = session; //直接存下来，省得去查
	h->channel = channel;
	strcpy(h->userId,switch_channel_get_variable(channel, "callee_id_number"));
	strcpy(h->callerId,switch_channel_get_variable(channel, "caller_id_number"));

	switch_threadattr_create(&thd_attr,switch_core_session_get_pool(session));
	switch_threadattr_detach_set(thd_attr, 1);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
	switch_thread_create(&thread, thd_attr, callqueue_thread_run, h,switch_core_session_get_pool(session));

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "will save queue handler id to hash with key:%s,queue id addr:0x%lu\n",uuid_str,(long)h->event_queue);
	if( SWITCH_STATUS_SUCCESS != switch_core_hash_insert_locked(globals.queue_hash,uuid_str,(void*)h->event_queue,globals.read_mutex))
	{
       switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "insert queue event list by key uuid:%s\n",uuid);
	   goto end;
	}


	/*应答通道*/
	switch_channel_answer(channel);
    /*一直放背景音乐*/
	while(switch_channel_ready(channel) && globals.running)
	{
		/* An agent was found, time to exit and let the bridge do it job */
		if ((p = switch_channel_get_variable(channel, "queue_agent_found")) && (agent_found = switch_true(p))) {
			break;
		}
	    /*清除私有队列中的消息*/ 
		switch_core_session_flush_private_events(session);
		/*播放背景音乐*/
		switch_ivr_play_file(session, NULL, globals.init_voice, NULL);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "*******************after play file***************\n");
		/*检查通道是否处于桥接状态，如果已经桥接，则退出线程处理...*/
		if (switch_channel_test_flag(channel, CF_BRIDGED)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "main thread call already transfer to agent\n");
			break;
		}
		switch_yield(10000);
	}
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "*******************exit app running******************\n");
end:	
	/*处理结束,销毁相关的资源...
	 删除hash中对应的key值*/
    if (globals.queue_hash)
    {
        switch_core_hash_delete_locked(globals.queue_hash,uuid_str,globals.read_mutex);
    }
	/*删除队列*/
	callqueue_flush_event_queue(h->event_queue);/*清除队列中的消息*/
    switch_queue_interrupt_all(h->event_queue);
	/*phandler不需要释放，session结束后，会自动释放掉。*/
	return ;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_callqueue_shutdown)
{
    switch_hash_index_t *hi = NULL;
	queue_deal_pcb_t *queue;
	void *val = NULL;
	const void *key;
	switch_ssize_t keylen;
	int sanity = 0;

    /*解除子事件注册*/
	switch_event_free_subclass(IR_EVENT_REQ);
	switch_event_free_subclass(CC_EVENT_REQ);
	switch_event_free_subclass(IR_EVENT_RES);
	switch_event_free_subclass(CC_EVENT_RES);
	switch_event_free_subclass(CC_EVENT_CANCEL);
	switch_event_free_subclass(IR_EVENT_CANCEL);
	switch_event_free_subclass(CALLQUEUE_TIMEOUT);
    /*事件退订*/
	switch_event_unbind(&globals.node);

	switch_mutex_lock(globals.mutex);
	if (globals.running == 1) {
		globals.running = 0;
	}
	switch_mutex_unlock(globals.mutex);

	while (globals.threads) {
		switch_cond_next();
		if (++sanity >= 60000) {
			break;
		}
	}

	switch_mutex_lock(globals.mutex);
	while ((hi = switch_core_hash_first_iter( globals.queue_hash, hi))) {
		switch_core_hash_this(hi, &key, &keylen, &val);
		queue = (queue_deal_pcb_t *) val;

		switch_core_hash_delete(globals.queue_hash, queue->session_uuid);
		queue->running = 0;

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Waiting for write lock (queue session uuid %s)\n", queue->session_uuid);
		/*handler data数据由session结束后自动释放，这里不做对应的释放....*/
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Destroying queue  session uuid %s\n", queue->session_uuid);
		queue = NULL;
	}
	
	/*释放内存...*/
	switch_safe_free(globals.init_voice);
	switch_safe_free(globals.success_voice);
	switch_safe_free(globals.full_voice);
	switch_safe_free(globals.queue_busy_voice);
	switch_safe_free(globals.agent_busy_voice);
	switch_safe_free(globals.agent_succ_voice);
	switch_safe_free(globals.cancel_voice);
	switch_safe_free(globals.error_voice);
	switch_safe_free(globals.no_angent_voice);
	switch_safe_free(globals.wait_number_voice);
	switch_safe_free(globals.user_break_voice);
	switch_safe_free(globals.queue_session_timeout_voice);
	 
	switch_mutex_unlock(globals.mutex);

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_callqueue_load)
{
	switch_application_interface_t *app_interface;
	switch_status_t status = SWITCH_STATUS_FALSE;

    /*注册ir::req*/
	if (switch_event_reserve_subclass(IR_EVENT_REQ) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass ir::req!");
		return SWITCH_STATUS_GENERR;
	}

	/*注册cc::req*/
	if (switch_event_reserve_subclass(CC_EVENT_REQ) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass cc::req!");
		return SWITCH_STATUS_GENERR;
	}

	/*注册cc::resp*/
    if (switch_event_reserve_subclass(CC_EVENT_RES) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass cc::resp!");
		return SWITCH_STATUS_GENERR;
	}

	/*注册ir::resp*/
    if (switch_event_reserve_subclass(IR_EVENT_RES) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass ir::resp!");
		return SWITCH_STATUS_GENERR;
	}

	/*注册ir::cancel*/
    if (switch_event_reserve_subclass(IR_EVENT_CANCEL) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass cc::cancel!");
		return SWITCH_STATUS_GENERR;
	}

	/*注释cc::cancel*/
    if (switch_event_reserve_subclass(CC_EVENT_CANCEL) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass cc::cancel!");
		return SWITCH_STATUS_GENERR;
	}

	/*注释cq::timeout*/
    if (switch_event_reserve_subclass(CALLQUEUE_TIMEOUT) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass cq::timeout!");
		return SWITCH_STATUS_GENERR;
	}

	memset(&globals, 0, sizeof(globals));
	globals.pool = pool;

	switch_mutex_init(&globals.mutex, SWITCH_MUTEX_NESTED, globals.pool);
	switch_mutex_init(&globals.read_mutex, SWITCH_MUTEX_NESTED, globals.pool);
	switch_core_hash_init(&globals.queue_hash);

	if ((status = load_config()) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "callqueue load config cfg error!\n");
		return status;
	}

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	if (switch_event_bind(modname, SWITCH_EVENT_ALL, SWITCH_EVENT_SUBCLASS_ANY, event_handler, &globals.node) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't bind!\n");
		return SWITCH_STATUS_GENERR;
	}
    SWITCH_ADD_APP(app_interface, "callqueue", "callqueue", CQ_DESC, queue_function, CQ_USAGE, SAF_SUPPORT_NOMEDIA);

   	switch_mutex_lock(globals.mutex);
	globals.running = 1;
	switch_mutex_unlock(globals.mutex);

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
