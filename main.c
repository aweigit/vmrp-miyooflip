#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "cJSON.h"

#include "./header/bridge.h"
#include "./header/vmrp.h"
#include "./header/memory.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define MOUSE_DOWN 2
#define MOUSE_UP 3
#define MOUSE_MOVE 12

#define SDL_EVENT_TIMER (SDL_USEREVENT + 1)

// http://wiki.libsdl.org/Tutorials
// http://lazyfoo.net/tutorials/SDL/index.php

static SDL_TimerID timeId = 0;

static SDL_Window *window;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *screenTexture = NULL;
static bool isMouseDown = false;
static bool isEditMode = false;
static int32_t editMaxSize = 0;
static char *holdEditText = NULL;
static SDL_Joystick *g_joystick=NULL;

#define DEFAULT_BGM_FREQ    44100
#define DEFAULT_BGM_FORMAT  AUDIO_S16LSB
#define DEFAULT_BGM_CHAN    2
#define DEFAULT_BGM_CHUNK   4096


static SDL_Keycode isKeyDown = SDLK_UNKNOWN;

void saveEditText(char *str) {
    uint8_t *utf8Str = (uint8_t *)str;
    int32_t n = 0;
    while (*utf8Str && (n < editMaxSize)) {
        if (*utf8Str < 0x80) {  // 1 Byte
            utf8Str += 1;
        } else if ((*utf8Str & 0xe0) == 0xc0) {  // 2 Bytes
            utf8Str += 2;
        } else if ((*utf8Str & 0xf0) == 0xe0) {  // 3 Bytes
            utf8Str += 3;
        } else {
            break;
        }
        n++;
    }
    if (holdEditText != NULL) {
        my_freeExt(holdEditText);
        holdEditText = NULL;
    }
    uint32_t len = (uintptr_t)utf8Str - (uintptr_t)str;
    holdEditText = my_mallocExt(len + 1);
    memcpy(holdEditText, str, len);
    holdEditText[len] = '\0';
}

int32_t editCreate(const char *title, const char *text, int32_t type, int32_t max_size) {
    isEditMode = true;
    editMaxSize = max_size;
    SDL_Log("title: '%s', text: '%s', type: %d, max_size: %d", title, text, type, max_size);
    if (SDL_SetClipboardText(text) == 0) {
        SDL_Log("编辑内容已复制到剪贴板，按ctrl+v输入内容，按ctrl+z取消");
    } else {
        SDL_Log("无法使用剪贴板");
    }
    return 1234;
}

int32 editRelease(int32 edit) {
    isEditMode = false;
    if (holdEditText != NULL) {
        my_freeExt(holdEditText);
        holdEditText = NULL;
    }
    return MR_SUCCESS;
}

char *editGetText(int32 edit) {
    SDL_Log("editGetText(): '%s'", holdEditText);
    return holdEditText;
}

void guiDrawBitmap(uint16_t *bmp, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!renderer) return;

    void* pixels;
    int pitch;
    if (SDL_LockTexture(screenTexture, NULL , &pixels, &pitch) == 0) {
        memcpy(pixels, (void *)bmp, 153600);

        SDL_UnlockTexture(screenTexture);

        SDL_RenderCopy(renderer, screenTexture, NULL, NULL);
        SDL_RenderPresent(renderer);
    } 
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
void setEventEnable(int v) {
    int state = v ? SDL_ENABLE : SDL_DISABLE;
    SDL_EventState(SDL_TEXTINPUT, state);
    SDL_EventState(SDL_KEYDOWN, state);
    SDL_EventState(SDL_KEYUP, state);
    SDL_EventState(SDL_MOUSEMOTION, state);
    SDL_EventState(SDL_MOUSEBUTTONDOWN, state);
    SDL_EventState(SDL_MOUSEBUTTONUP, state);
}
#endif

uint32_t timerCb(uint32_t interval, void *param) {
    SDL_RemoveTimer(timeId);
    timeId = 0;
    SDL_Event ev;
    ev.type = SDL_EVENT_TIMER;
    SDL_PushEvent(&ev);
    return 0;
}

int32_t timerStart(uint16_t t) {
    if (!timeId) {
        timeId = SDL_AddTimer(t, timerCb, NULL);
    } else {
        SDL_RemoveTimer(timeId);
        timeId = SDL_AddTimer(t, timerCb, NULL);
    }
    //printf("t:%d\n",t);
    return MR_SUCCESS;
}

int32_t timerStop() {
    if (timeId) {
        SDL_RemoveTimer(timeId);
        timeId = 0;
    }
    return MR_SUCCESS;
}

static void keyEvent(int16 type, SDL_Keycode code) {
    if (code >= SDLK_0 && code <= SDLK_9) {
        int32_t key = MR_KEY_0 + (code - SDLK_0);
        event(type, key, 0);  // 按键 0-9
        return;
    }
    switch (code) {
        case SDLK_KP_0:
            event(type, MR_KEY_0, 0);
            break;
        case SDLK_KP_1:
            event(type, MR_KEY_1, 0);
            break;
        case SDLK_KP_2:
            event(type, MR_KEY_2, 0);
            break;
        case SDLK_KP_3:
            event(type, MR_KEY_3, 0);
            break;
        case SDLK_KP_4:
            event(type, MR_KEY_4, 0);
            break;
        case SDLK_KP_5:
            event(type, MR_KEY_5, 0);
            break;
        case SDLK_KP_6:
            event(type, MR_KEY_6, 0);
            break;
        case SDLK_KP_7:
            event(type, MR_KEY_7, 0);
            break;
        case SDLK_KP_8:
            event(type, MR_KEY_8, 0);
            break;
        case SDLK_KP_9:
            event(type, MR_KEY_9, 0);
            break;
        case SDLK_KP_ENTER:
        case SDLK_RETURN:                   // 回车键
            event(type, MR_KEY_SELECT, 0);  // 确认/选择/ok
            break;
        case SDLK_EQUALS:                  // 等号
            event(type, MR_KEY_POUND, 0);  // 按键 #
            break;
        case SDLK_MINUS:                  // 减号
            event(type, MR_KEY_STAR, 0);  // 按键 *
            break;
        case SDLK_w:
        case SDLK_UP:  // 上
            event(type, MR_KEY_UP, 0);
            break;
        case SDLK_s:
        case SDLK_DOWN:  // 下
            event(type, MR_KEY_DOWN, 0);
            break;
        case SDLK_a:
        case SDLK_LEFT:  // 左
            event(type, MR_KEY_LEFT, 0);
            break;
        case SDLK_d:
        case SDLK_RIGHT:  // 右
            event(type, MR_KEY_RIGHT, 0);
            break;
        case SDLK_q:
        case SDLK_LEFTBRACKET:                // 左中括号
            event(type, MR_KEY_SOFTLEFT, 0);  // 左功能键
            break;
        case SDLK_e:
        case SDLK_RIGHTBRACKET:                // 右中括号
            event(type, MR_KEY_SOFTRIGHT, 0);  // 右功能键
            break;
        case SDLK_TAB:
            event(type, MR_KEY_SEND, 0);  // 接听键
            break;
        case SDLK_ESCAPE:
            event(type, MR_KEY_POWER, 0);  // 挂机键
            break;
        default:
            printf("key:%d\n", code);
            break;
    }
}

#define SDL_AXIS_TRIGGERLEFT 100
#define SDL_AXIS_TRIGGERRIGHT 101

int rotate = 0;
bool use_mouse = 0;
bool use_numpad = 0;
//模拟器按键与sdl键值对应关系
int KEY_LEFT=SDLK_q;
int KEY_RIGHT=SDLK_e;
int KEY_OK=SDLK_RETURN;
int KEY_STAR=SDLK_MINUS;//-
int KEY_POUND=SDLK_EQUALS;//=
int KEY_1=SDLK_1;
int KEY_3=SDLK_3;
int KEY_7=SDLK_7;
int KEY_9=SDLK_9;
int KEY_0=SDLK_0;

//这个地方会根据配置变化
int BUTTON_Y;
int BUTTON_A;
int BUTTON_X;
int BUTTON_BACK;
int BUTTON_START;
int BUTTON_LEFTSHOULDER;
int BUTTON_RIGHTSHOULDER;
int TRIGGERLEFT;
int TRIGGERRIGHT;
int BUTTON_B;

#define STICK_DEAD_ZONE 8000  // 死区阈值，可调
#define TRIGGER_PRESS_THRESHOLD   0x7000  // 按下阈值
#define TRIGGER_RELEASE_THRESHOLD 0x5000  // 释放阈值（必须 <= PRESS）
// 配置参数（可调整）
#define DEAD_ZONE      0.15f   // 摇杆死区（0.0 ～ 1.0）
#define SENSITIVITY    4.0f    // 每帧最大移动像素（越高越快）
#define USE_RADIAL_DZ  1       // 1 = 径向死区，0 = 轴向死区
static bool left_trigger_active  = false;
static bool right_trigger_active = false;

// 当前方向状态（避免重复打印/触发）
static bool up    = false;
static bool down  = false;
static bool left  = false;
static bool right = false;

// 存储当前轴值（用于帧更新或事件驱动）
static Sint16 left_x = 0;
static Sint16 left_y = 0;

void initKey()
{
    //这个地方会根据配置变化
    BUTTON_Y=KEY_LEFT;
    BUTTON_A=KEY_RIGHT;
    BUTTON_X=KEY_OK;
    BUTTON_BACK=KEY_STAR;
    BUTTON_START=KEY_POUND;
    BUTTON_LEFTSHOULDER=KEY_1;
    BUTTON_RIGHTSHOULDER=KEY_3;
    TRIGGERLEFT=KEY_7;
    TRIGGERRIGHT=KEY_9;
    BUTTON_B=KEY_0;
}



int joy2emu(int joy)
{
	switch(joy)
	{
		case SDL_CONTROLLER_BUTTON_Y:
			return BUTTON_X;
		case SDL_CONTROLLER_BUTTON_A:
			return BUTTON_B;
		case SDL_CONTROLLER_BUTTON_X:
			return BUTTON_Y;
		case SDL_CONTROLLER_BUTTON_BACK:
			return BUTTON_BACK;
		case SDL_CONTROLLER_BUTTON_START:
			return BUTTON_START;
		case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
			return BUTTON_LEFTSHOULDER;
		case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
			return BUTTON_RIGHTSHOULDER;
		case SDL_AXIS_TRIGGERLEFT://注意这是自定义的摇杆的轴值，确认是否跟Button值重复
			return TRIGGERLEFT;
		case SDL_AXIS_TRIGGERRIGHT:
			return TRIGGERRIGHT;
		case SDL_CONTROLLER_BUTTON_B:
			return BUTTON_A;
	}
	
	return 0;
}

void sendKey_Numpad(int key , bool pressed)
{
	int newkey=0;
	switch (key)
	{
		case SDL_CONTROLLER_BUTTON_DPAD_UP:
			newkey = SDLK_2;
			break;
		case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
			newkey = SDLK_8;
			break;
		case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
			newkey = SDLK_4;
			break;
		case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
			newkey = SDLK_6;
			break;
		default:
			return;
	}

    keyEvent(pressed ? MR_KEY_PRESS : MR_KEY_RELEASE, newkey);
}

void sendKey_Direct(int key , bool pressed)
{
	int newkey=0;
	switch (key)
	{
		case SDL_CONTROLLER_BUTTON_DPAD_UP:
			newkey = (rotate ==0 ? SDLK_UP : (rotate == 2 ? SDLK_RIGHT : SDLK_LEFT ));
			break;
		case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
			newkey = (rotate ==0 ? SDLK_DOWN : (rotate == 2 ? SDLK_LEFT : SDLK_RIGHT));
			break;
		case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
			newkey = (rotate ==0 ? SDLK_LEFT : (rotate == 2 ? SDLK_UP : SDLK_DOWN));
			break;
		case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
			newkey = (rotate ==0 ? SDLK_RIGHT : (rotate == 2 ? SDLK_DOWN : SDLK_UP));
			break;
		default:
			return;
	}

	// if(use_mouse)
	// {
	// 	updateMouse(newkey);
	// }
	// else
	// {
	// 	sendKey(newkey, pressed);
	// }
    keyEvent(pressed ? MR_KEY_PRESS : MR_KEY_RELEASE, newkey);
}

void update_direction_from_stick() {
    bool new_up    = (left_y < -STICK_DEAD_ZONE);
    bool new_down  = (left_y >  STICK_DEAD_ZONE);
    bool new_left  = (left_x < -STICK_DEAD_ZONE);
    bool new_right = (left_x >  STICK_DEAD_ZONE);

    // 检测“按下”（从 false → true）
    if (new_up && !up){
		printf("UP pressed\n");
		sendKey_Direct(SDL_CONTROLLER_BUTTON_DPAD_UP, true);
	}    
    if (new_down && !down){
		printf("DOWN pressed\n");
		sendKey_Direct(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true);
	} 
    if (new_left && !left){
		printf("LEFT pressed\n");
		sendKey_Direct(SDL_CONTROLLER_BUTTON_DPAD_LEFT, true);
	} 
    if (new_right && !right)
	{
		printf("RIGHT pressed\n");
		sendKey_Direct(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, true);
	} 

    // 检测“释放”（从 true → false）
    if (!new_up && up){
		printf("UP released\n");
		sendKey_Direct(SDL_CONTROLLER_BUTTON_DPAD_UP, false);
	}    
    if (!new_down && down){
		printf("DOWN released\n");
		sendKey_Direct(SDL_CONTROLLER_BUTTON_DPAD_DOWN, false);
	} 
    if (!new_left && left){
		printf("LEFT released\n");
		sendKey_Direct(SDL_CONTROLLER_BUTTON_DPAD_LEFT, false);
	} 
    if (!new_right && right){
		printf("RIGHT released\n");
		sendKey_Direct(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, false);
	} 

    // 更新状态
    up    = new_up;
    down  = new_down;
    left  = new_left;
    right = new_right;
}


void loop() {
    SDL_Event ev;
    bool isLoop = true;
    int mod = 0;
    uint32_t lastTimer = SDL_GetTicks();

    while (isLoop) {
        while (isLoop && SDL_WaitEvent(&ev)){
            if (ev.type == SDL_QUIT) {
                isLoop = false;
                break;
            }
            if (isEditMode) {
                switch (ev.type) {
                    case SDL_EVENT_TIMER:
                        timer();
                        break;
                    case SDL_KEYDOWN: {
                        if (SDL_GetModState() & KMOD_CTRL) {
                            if (ev.key.keysym.sym == SDLK_z) {
                                event(MR_DIALOG_EVENT, 1, 0);
                                SDL_Log("取消输入");
                                continue;
                            } else if (ev.key.keysym.sym == SDLK_v) {
                                char *str = SDL_GetClipboardText();
                                saveEditText(str);
                                SDL_free(str);
                                event(MR_DIALOG_EVENT, 0, 0);
                                continue;
                            }
                        }
                    }
                    case SDL_MOUSEBUTTONDOWN:
                        SDL_Log("ctrl+v输入内容，ctrl+z取消输入");
                }
                continue;
            }
            switch (ev.type) {
                case SDL_EVENT_TIMER:
                    timer();
                    break;
                case SDL_KEYDOWN:
                    if (isKeyDown == SDLK_UNKNOWN) {
                        isKeyDown = ev.key.keysym.sym;
                        keyEvent(MR_KEY_PRESS, ev.key.keysym.sym);
                    }
                    break;
                case SDL_KEYUP:
                    if (isKeyDown == ev.key.keysym.sym) {
                        isKeyDown = SDLK_UNKNOWN;
                        keyEvent(MR_KEY_RELEASE, ev.key.keysym.sym);
                    }
                    break;
                case SDL_MOUSEMOTION:
                    if (isMouseDown) {
                        event(MR_MOUSE_MOVE, ev.motion.x, ev.motion.y);
                    }
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    isMouseDown = true;
                    event(MR_MOUSE_DOWN, ev.motion.x, ev.motion.y);
                    break;
                case SDL_MOUSEBUTTONUP:
                    isMouseDown = false;
                    event(MR_MOUSE_UP, ev.motion.x, ev.motion.y);
                    break;
                case SDL_CONTROLLERAXISMOTION: {
                    Sint16 value = ev.caxis.value;
					//const char* axis_name = "";
					
					switch (ev.caxis.axis) {
						case SDL_CONTROLLER_AXIS_LEFTX://左摇杆用于控制方向
							left_x = value;
							update_direction_from_stick();
							//axis_name = "Left Stick X";
							break;
						case SDL_CONTROLLER_AXIS_LEFTY:
							left_y = value;
							update_direction_from_stick();
							//axis_name = "Left Stick Y";
							break;
						case SDL_CONTROLLER_AXIS_RIGHTX:
							//axis_name = "Right Stick X";
							break;
						case SDL_CONTROLLER_AXIS_RIGHTY:
							//axis_name = "Right Stick Y";
							break;
						case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
						{
							if (!left_trigger_active && value > TRIGGER_PRESS_THRESHOLD) {
								left_trigger_active = true;
								//printf("Left Trigger PRESSED (value=%d)\n", value);
								// 在这里执行“按下”逻辑
								keyEvent(MR_KEY_PRESS, joy2emu(SDL_AXIS_TRIGGERLEFT));
							}
							else if (left_trigger_active && value < TRIGGER_RELEASE_THRESHOLD) {
								left_trigger_active = false;
								//printf("Left Trigger RELEASED (value=%d)\n", value);
								// 在这里执行“释放”逻辑
								keyEvent(MR_KEY_RELEASE, joy2emu(SDL_AXIS_TRIGGERLEFT));
							}
						
							//axis_name = "Left Trigger";
							break;
						}
						case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
						{
							if (!right_trigger_active && value > TRIGGER_PRESS_THRESHOLD) {
								right_trigger_active = true;
								printf("Right Trigger PRESSED (value=%d)\n", value);
								keyEvent(MR_KEY_PRESS, joy2emu(SDL_AXIS_TRIGGERRIGHT));
							}
							else if (right_trigger_active && value < TRIGGER_RELEASE_THRESHOLD) {
								right_trigger_active = false;
								printf("Right Trigger RELEASED (value=%d)\n", value);
								keyEvent(MR_KEY_RELEASE, joy2emu(SDL_AXIS_TRIGGERRIGHT));
							}
						
							//axis_name = "Right Trigger";
							break;
						}
						default:
							//axis_name = "Unknown Axis";
                            break;
					}
					
					//printf("axis_name: %s value:%d\n", axis_name, e.caxis.value);
					//wout("keytest.txt", axis_name, e.caxis.value);
					
					break;
                }
				

                case SDL_CONTROLLERBUTTONDOWN:
                case SDL_CONTROLLERBUTTONUP: {
					//const char* button_name = "";
                    //const char* state = (ev.cbutton.state == SDL_PRESSED) ? " 按下 " : " 释放 ";
                    
					int key=0;
					switch (ev.cbutton.button) {
						case SDL_CONTROLLER_BUTTON_A:
							//button_name="SDL_CONTROLLER_BUTTON_A";
							break;
						case SDL_CONTROLLER_BUTTON_B:
							//button_name="SDL_CONTROLLER_BUTTON_B";
							break;
						case SDL_CONTROLLER_BUTTON_X:
							//button_name="SDL_CONTROLLER_BUTTON_X";
							break;
						case SDL_CONTROLLER_BUTTON_Y://在鼠标模式下模拟鼠标点击
							//button_name="SDL_CONTROLLER_BUTTON_Y";
							// if(use_mouse){
							// 	sendKey_Mouse(e.cbutton.state == SDL_PRESSED);
							// 	e.cbutton.button=0;
							// }
							break;
						case SDL_CONTROLLER_BUTTON_BACK: //select
							//button_name="SDL_CONTROLLER_BUTTON_BACK";
							break;
						case SDL_CONTROLLER_BUTTON_START: //start
							//button_name="SDL_CONTROLLER_BUTTON_START";
							break;
						case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: //L1
							//button_name="SDL_CONTROLLER_BUTTON_LEFTSHOULDER";
							break;
						case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: //R1
							//button_name="SDL_CONTROLLER_BUTTON_RIGHTSHOULDER";
							break;
						case SDL_CONTROLLER_BUTTON_LEFTSTICK://左摇杆按下把方向键切换为2,4,6,8
							//button_name="SDL_CONTROLLER_BUTTON_LEFTSTICK";
							use_numpad=((ev.cbutton.state == SDL_PRESSED) ? (1-use_numpad) : use_numpad) ;
							break;
						case SDL_CONTROLLER_BUTTON_RIGHTSTICK://右摇杆作为ok键(SDLK_o)
							//button_name="SDL_CONTROLLER_BUTTON_RIGHTSTICK";
							keyEvent(ev.cbutton.state == SDL_PRESSED ? MR_KEY_PRESS : MR_KEY_RELEASE, SDLK_RETURN);
							break;
						case SDL_CONTROLLER_BUTTON_DPAD_UP:
						case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
						case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
						case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
							if(use_numpad)
							{
								sendKey_Numpad(ev.cbutton.button, ev.cbutton.state == SDL_PRESSED);
							}
							else{
								sendKey_Direct(ev.cbutton.button, ev.cbutton.state == SDL_PRESSED);
							}
							break;
						case SDL_CONTROLLER_BUTTON_GUIDE: //menu键退出
							//button_name="SDL_CONTROLLER_BUTTON_GUIDE";
							isLoop = false;
							break;
						default://未识别的键也退出
							//button_name = "Unknown button_name";
							isLoop = false;
							break;	
						
					}
					//std::cout << "按钮 [" << e.cbutton.button << "] " << button_name << state << "\n";
					//wout("keytest.txt", button_name, e.cbutton.button);
					//按住select键
					if(ev.cbutton.button == SDL_CONTROLLER_BUTTON_BACK && ev.cbutton.state == SDL_PRESSED){mod=1;}
					else if(ev.cbutton.button == SDL_CONTROLLER_BUTTON_BACK && ev.cbutton.state == SDL_RELEASED){mod=0;}

					key = joy2emu(ev.cbutton.button);

					if(mod)
					{
						switch (ev.cbutton.button) {
							case SDL_CONTROLLER_BUTTON_X://切换鼠标模式
								mod=0;
								use_mouse=1-use_mouse;//切换鼠标
								key=0;
								break;
							case SDL_CONTROLLER_BUTTON_A://旋转
								mod=0;
								rotate=(1+rotate)%3;//连续旋转
								key=0;
								break;
							case SDL_CONTROLLER_BUTTON_START://切换按键输入模式
								mod=0;
								key=0;
								break;
						}
					}

					if(key && !use_mouse)
					{
						keyEvent(ev.cbutton.state == SDL_PRESSED ? MR_KEY_PRESS : MR_KEY_RELEASE, key);
					}

					break;
                }
            }
            
        }

    }
}

void clean()
{
    if(g_joystick)
	{
		SDL_JoystickClose(g_joystick);
	}
	SDL_DestroyTexture(screenTexture);
	SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    Mix_CloseAudio();
    Mix_Quit();
    SDL_Quit();
}

void setEmuKey(const char * name, int key)
{
	if(strcmp(name,"X")==0)
	{
		BUTTON_X=key;
	}
	else if(strcmp(name,"B")==0)
	{
		BUTTON_B=key;
	}
	else if(strcmp(name,"A")==0)
	{
		BUTTON_A=key;
	}
	else if(strcmp(name,"SELECT")==0)
	{
		BUTTON_BACK=key;
	}
	else if(strcmp(name,"START")==0)
	{
		BUTTON_START=key;
	}
	else if(strcmp(name,"Y")==0)
	{
		BUTTON_Y=key;
	}
	else if(strcmp(name,"L")==0)
	{
		BUTTON_LEFTSHOULDER=key;
	}
	else if(strcmp(name,"R")==0)
	{
		BUTTON_RIGHTSHOULDER=key;
	}
	else if(strcmp(name,"L2")==0)
	{
		TRIGGERLEFT=key;
	}
	else if(strcmp(name,"R2")==0)
	{
		TRIGGERRIGHT=key;
	}
	
}

int loadConfig() {
    // 打开文件
    FILE *file = fopen("keymap.cfg", "r");
    if (file == NULL) {
		printf("打开文件失败\n");
        return -1;
    }
 
    // 确定文件长度
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
 
    // 读取文件内容到字符串
    char *data = (char*)malloc(length + 1);
    fread(data, 1, length, file);
    data[length] = '\0';
    fclose(file);
 
    // 解析JSON字符串
    cJSON *json = cJSON_Parse(data);
    if (json == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            printf("解析json错误:%s\n", error_ptr);
        }
        cJSON_Delete(json);
        free(data);
        return -1;
    }
	
    // 使用cJSON对象
    cJSON *name = cJSON_GetObjectItem(json, "左键");
    setEmuKey(name->valuestring, KEY_LEFT);
    
    name = cJSON_GetObjectItem(json, "右键");
    setEmuKey(name->valuestring, KEY_RIGHT);
    
    name = cJSON_GetObjectItem(json, "OK");
    setEmuKey(name->valuestring, KEY_OK);
    
    name = cJSON_GetObjectItem(json, "*");
    setEmuKey(name->valuestring, KEY_STAR);
    
    name = cJSON_GetObjectItem(json, "#");
    setEmuKey(name->valuestring, KEY_POUND);
    
    name = cJSON_GetObjectItem(json, "0");
    setEmuKey(name->valuestring, KEY_0);
    
    name = cJSON_GetObjectItem(json, "1");
    setEmuKey(name->valuestring, KEY_1);
    
    name = cJSON_GetObjectItem(json, "3");
    setEmuKey(name->valuestring, KEY_3);
    
    name = cJSON_GetObjectItem(json, "7");
    setEmuKey(name->valuestring, KEY_7);
    
    name = cJSON_GetObjectItem(json, "9");
    setEmuKey(name->valuestring, KEY_9);
 
    // 清理工作
    cJSON_Delete(json);
    free(data);
 
    return 0;
}

int main(int argc, char *argv[]) {
    const char *mrpFile = "dsm_gm.mrp";
    const char *extName = "start.mr";
    const char *entry = NULL;
    int machine_width = 640;
    int machine_height = 480;

    if (argc < 3) {
        printf("Usage: %s <width> <height> [mrp_file] [ext_name] [entry]\n", argv[0]);
        printf("  mrp_file : .mrp file to launch (default: dsm_gm.mrp)\n");
        printf("  ext_name : entry point file   (default: start.mr)\n");
        printf("  entry    : entry function     (default: NULL)\n\n");
        return 0;
    }
    machine_width = strtol(argv[1], NULL, 10);
    machine_height = strtol(argv[2], NULL, 10);
    if (argc >= 4) mrpFile = argv[3];
    if (argc >= 5) extName = argv[4];
    if (argc >= 6) entry = argv[5];

#ifdef __x86_64__
    printf("__x86_64__\n");
#elif __i386__
    printf("__i386__\n");
#elif __aarch64__
    printf("__aarch64__\n");
#elif __arm__
    printf("__arm__\n");
#endif

    printf("CODE_ADDRESS:0x%X, CODE_SIZE:0x%X\n", CODE_ADDRESS, CODE_SIZE);
    printf("STACK_ADDRESS:0x%X, STACK_SIZE:0x%X\n", STACK_ADDRESS, STACK_SIZE);
    printf("MEMORY_MANAGER_ADDRESS:0x%X, MEMORY_MANAGER_SIZE:0x%X\n", MEMORY_MANAGER_ADDRESS, MEMORY_MANAGER_SIZE);
    printf("START_ADDRESS:0x%X, END_ADDRESS:0x%X\n", START_ADDRESS, END_ADDRESS);
    printf("TOTAL_MEMORY:0x%X(%d)\n", TOTAL_MEMORY, TOTAL_MEMORY);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        printf("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        return -1;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");//最近邻

    // 初始化SDL_mixer
    if (Mix_OpenAudio(DEFAULT_BGM_FREQ, DEFAULT_BGM_FORMAT, DEFAULT_BGM_CHAN, DEFAULT_BGM_CHUNK) < 0) {
        printf("Mix_OpenAudio err: %s\n", Mix_GetError());
    }

    window = SDL_CreateWindow("vmrp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, machine_width, machine_height,  SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (window == NULL) {
        printf("Window could not be created! SDL_Error: %s\n", SDL_GetError());
        return -1;
    }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        printf("SDL_CreateRenderer err: %s\n", SDL_GetError());
        return -1;
    }
    SDL_RenderSetLogicalSize(renderer, SCREEN_WIDTH, SCREEN_HEIGHT);
    //SDL_RenderSetIntegerScale(renderer, SDL_TRUE);//整数倍缩放
    screenTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
        SDL_TEXTUREACCESS_STREAMING, SCREEN_WIDTH, SCREEN_HEIGHT);
    if (!screenTexture) {
        printf("SDL_CreateTexture err: %s\n", SDL_GetError());
        return -1;
    }

    SDL_GameControllerEventState(SDL_ENABLE);

    // 检测启动时已连接的手柄
    int num_joysticks = SDL_NumJoysticks();
    if(num_joysticks>0)
    {
        SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(0);
        
        // 转为字符串（格式：00000000000000000000000000000000）
        char guid_str[33];
        SDL_JoystickGetGUIDString(guid, guid_str, sizeof(guid_str));

        const char* name = SDL_JoystickNameForIndex(0);
        printf("Joystick: %s\n", name);
        printf("  GUID: %s\n", guid_str);
        if (SDL_IsGameController(0)) {
            g_joystick = SDL_GameControllerOpen(0);
            if (g_joystick) {
                SDL_JoystickID instance_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_joystick));
                printf("手柄已连接: %s (ID: %d )\n", SDL_GameControllerName(g_joystick), instance_id);
            } else {
                printf("无法打开手柄: %s\n", SDL_GetError());
            }
        } else {
            printf("不是有效的 Game Controller\n");
        }
    }

    initKey();
    loadConfig();
    startVmrp(mrpFile, extName, entry);

#if defined(__EMSCRIPTEN__)
    emscripten_set_main_loop(loop, 0, 1);
#else
    loop();
    clean();
#endif
    return 0;
}
