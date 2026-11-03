/* USER CODE BEGIN Header */
/* STM32G474RE 1A2B 猜數字遊戲主程式。
 * Copyright (c) 2026 STMicroelectronics. All rights reserved.
 * 原廠授權條款請參閱專案 LICENSE；若無授權檔，程式依現況提供。
 */
/* USER CODE END Header */
/* 引用標頭檔 */
#include "main.h"
/* 額外引用的標頭檔 */
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */
/* 自訂資料型別 */
/* USER CODE BEGIN PTD */
/* USER CODE END PTD */
/* 常數設定 */
/* USER CODE BEGIN PD */
/* ===== 遊戲與硬體參數 =====
 * GAME_LEN: 猜測長度為 4 位
 * RX_TIMEOUT_MS: 每次輪詢 UART 最多等 1 ms，避免主迴圈卡住
 * DEBOUNCE_MS: 按鈕防彈跳時間 200 ms
 * BREATH_STEP_MS: 每 10 ms 更新一次亮度
 * PWM_TOP: TIM3 ARR=999，CCR 0~999 對應約 0~100% 亮度
 */
#define GAME_LEN          4U
#define RX_TIMEOUT_MS     1U
#define DEBOUNCE_MS       200U
#define BREATH_STEP_MS    10U
#define PWM_TOP           999U
/* USER CODE END PD */
/* 巨集定義 */
/* USER CODE BEGIN PM */
/* USER CODE END PM */
/* 全域與靜態變數 */
UART_HandleTypeDef hlpuart1;
TIM_HandleTypeDef htim3;
/* USER CODE BEGIN PV */
/* ===== 遊戲資料 =====
 * answer / previous_answer: 當局與前一局的答案（每位以數值 0~9 儲存）
 * game_started: 第一次玩家操作後才產生答案
 * guess_round: 當局有效猜測次數
 * rng_state: xorshift 偽隨機產生器的內部狀態
 */
static const uint32_t led_channels[GAME_LEN] = {
  TIM_CHANNEL_1, TIM_CHANNEL_2, TIM_CHANNEL_3, TIM_CHANNEL_4
};
static uint8_t answer[GAME_LEN];
static uint8_t previous_answer[GAME_LEN];
static uint8_t have_previous_answer = 0U;
static uint8_t game_started = 0U;
static uint8_t game_won = 0U;
static uint32_t guess_round = 0U;
static uint32_t rng_state = 1U;
static uint8_t rng_seeded = 0U;
static uint8_t input_digits[GAME_LEN];
static uint8_t input_length = 0U;
static uint8_t input_invalid = 0U;
static uint8_t last_was_cr = 0U;
static uint8_t led_breath[GAME_LEN] = {0U};
static uint16_t breath_value = 0U;
static int8_t breath_direction = 1;
static uint32_t last_breath_tick = 0U;
volatile uint8_t restart_requested = 0U;
static volatile uint32_t last_button_tick = 0U;
static volatile uint8_t button_seen = 0U;
static volatile uint32_t first_button_tick = 0U;
static volatile uint32_t first_button_cycle = 0U;
/* USER CODE END PV */
/* 函式宣告 */
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_LPUART1_UART_Init(void);
static void MX_TIM3_Init(void);
/* USER CODE BEGIN PFP */
static void UART_Print(const char *message);
static void Seed_RNG(uint32_t tick, uint32_t cycles);
static uint32_t Random_Next(void);
static void Generate_Answer(void);
static void Start_New_Game(uint8_t reveal_old);
static void PWM_Set(uint8_t index, uint16_t duty);
static void LEDs_Clear(void);
static void LEDs_Show_Result(const uint8_t guess[GAME_LEN]);
static void Breath_Update(void);
static void Process_Line(void);
static void Process_Byte(uint8_t byte);
/* USER CODE END PFP */
/* 使用者自訂程式 */
/* USER CODE BEGIN 0 */
/* ===== 遊戲核心函式 =====
 * UART 使用輪詢接收；此區負責回覆訊息、產生答案、更新 LED 與判定輸入。
 */
/* 透過 LPUART1 傳送 ASCII 字串；格式包含 CRLF，方便 GUI 逐行解析。 */
static void UART_Print(const char *message)
{
  size_t length = strlen(message);
  if (length != 0U)
  {
    (void)HAL_UART_Transmit(&hlpuart1, (uint8_t *)message, (uint16_t)length, 1000U);
  }
}
/* 只在首次操作時設定種子，混合系統毫秒數與 DWT 週期數。 */
static void Seed_RNG(uint32_t tick, uint32_t cycles)
{
  if (rng_seeded != 0U) return;
  rng_state = 0x9E3779B9UL ^ tick ^ (cycles * 0x85EBCA6BUL);
  if (rng_state == 0U) rng_state = 0x6D2B79F5UL;
  rng_seeded = 1U;
  (void)Random_Next();
}
/* xorshift32 偽隨機演算法：快速產生下一個 32 位元數。 */
static uint32_t Random_Next(void)
{
  uint32_t x = rng_state; /* 以目前狀態計算下一個偽隨機值。 */
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng_state = x;
  return x;
}
/* 從 0~9 數字池抽取不重複的四位答案，允許 0 開頭；避開上一局完全相同的答案。 */
static void Generate_Answer(void)
{
  uint8_t pool[10];
  uint8_t candidate[GAME_LEN];
  uint8_t duplicate;
  do
  {
    for (uint8_t i = 0U; i < 10U; ++i) pool[i] = i;
    for (uint8_t i = 0U; i < GAME_LEN; ++i)
    {
      uint8_t j = (uint8_t)(i + (Random_Next() % (10U - i))); /* 從剩餘數字選一個。 */
      uint8_t swap = pool[i];
      pool[i] = pool[j];
      pool[j] = swap;
      candidate[i] = pool[i];
    }
    duplicate = (uint8_t)(have_previous_answer &&
                         memcmp(candidate, previous_answer, GAME_LEN) == 0);
  } while (duplicate != 0U);
  memcpy(answer, candidate, GAME_LEN);
  memcpy(previous_answer, candidate, GAME_LEN); /* 保存本局，供下局避開重複。 */
  have_previous_answer = 1U;
}
/* 設定指定 LED 的 TIM3 比較值 CCR；0 熄滅，999 接近全亮。 */
static void PWM_Set(uint8_t index, uint16_t duty)
{
  if (index >= GAME_LEN) return;
  if (duty > PWM_TOP) duty = PWM_TOP;
  __HAL_TIM_SET_COMPARE(&htim3, led_channels[index], duty);
}
/* 清除四顆 LED 的呼吸標記，設定 PWM 為 0 並重設呼吸亮度。 */
static void LEDs_Clear(void)
{
  for (uint8_t i = 0U; i < GAME_LEN; ++i)
  {
    led_breath[i] = 0U;
    PWM_Set(i, 0U);
  }
  breath_value = 0U;
  breath_direction = 1;
  last_breath_tick = HAL_GetTick();
}
/* 逐一比較輸入位置：A（數字位置均正確）常亮；B（數字正確但位置錯誤）呼吸；其他熄滅。 */
static void LEDs_Show_Result(const uint8_t guess[GAME_LEN])
{
  for (uint8_t i = 0U; i < GAME_LEN; ++i)
  {
    uint8_t match_a = (uint8_t)(guess[i] == answer[i]); /* 數字及位置都正確。 */
    uint8_t match_b = 0U;
    if (match_a == 0U)
    {
      for (uint8_t j = 0U; j < GAME_LEN; ++j)
      {
        if (guess[i] == answer[j]) { match_b = 1U; break; }
      }
    }
    led_breath[i] = match_b; /* B 對應 LED 啟用呼吸效果。 */
    PWM_Set(i, (match_a != 0U) ? PWM_TOP :
               ((match_b != 0U) ? breath_value : 0U));
  }
}
/* 依 HAL_GetTick() 每 10 ms 增減約 10 個 PWM 單位；約 1 秒漸亮及 1 秒漸暗。 */
static void Breath_Update(void)
{
  uint32_t now = HAL_GetTick();
  while ((uint32_t)(now - last_breath_tick) >= BREATH_STEP_MS)
  {
    last_breath_tick += BREATH_STEP_MS; /* 固定步進，減少更新頻率波動。 */
    if (breath_direction > 0)
    {
      if (breath_value >= 990U) { breath_value = PWM_TOP; breath_direction = -1; }
      else breath_value += 10U;
    }
    else
    {
      if (breath_value <= 10U) { breath_value = 0U; breath_direction = 1; }
      else breath_value -= 10U;
    }
  }
  for (uint8_t i = 0U; i < GAME_LEN; ++i)
  {
    if (led_breath[i] != 0U) PWM_Set(i, breath_value);
  }
}
/* 若是按鈕重開，先輸出舊答案；再產生新答案，清除回合計數、輸入狀態及 LED。 */
static void Start_New_Game(uint8_t reveal_old)
{
  char line[64];
  if ((reveal_old != 0U) && (game_started != 0U))
  {
    (void)snprintf(line, sizeof(line), "Answer: %u%u%u%u\r\n",
                   answer[0], answer[1], answer[2], answer[3]);
    UART_Print(line);
  }
  Generate_Answer();
  game_started = 1U;
  game_won = 0U;
  guess_round = 0U;
  input_length = 0U;
  input_invalid = 0U;
  last_was_cr = 0U;
  LEDs_Clear();
  /* 第一局不主動輸出訊息，避免 UART GUI 首次傳送時漏收字元。 */
  if (reveal_old != 0U) UART_Print("New game!\r\n");
}
/* 遇到換行後檢查輸入，計算 A/B，回傳 Round N: XXXX -> xAyB 並更新四顆 LED。 */
static void Process_Line(void)
{
  uint8_t count_a = 0U;
  uint8_t count_b = 0U;
  char line[64];
  if ((input_length == 0U) && (input_invalid == 0U)) return;
  if ((input_invalid != 0U) || (input_length != GAME_LEN))
  {
    UART_Print("Invalid input! Enter 4 different digits.\r\n");
    return;
  }
  if (game_won != 0U)
  {
    UART_Print("Game finished. Press button to restart.\r\n");
    return;
  }
  for (uint8_t i = 0U; i < GAME_LEN; ++i)
  {
    if (input_digits[i] == answer[i]) ++count_a; /* 位置相同即為 A。 */
    else
    {
      for (uint8_t j = 0U; j < GAME_LEN; ++j)
      {
        if (input_digits[i] == answer[j]) { ++count_b; break; }
      }
    }
  }
  ++guess_round; /* 僅計入格式正確的猜測。 */
  (void)snprintf(line, sizeof(line), "Round %lu: %u%u%u%u -> %uA%uB\r\n",
                 (unsigned long)guess_round,
                 input_digits[0], input_digits[1], input_digits[2], input_digits[3],
                 count_a, count_b);
  UART_Print(line);
  LEDs_Show_Result(input_digits);
  if (count_a == GAME_LEN) game_won = 1U;
}
/* 逐字元接收 0~9；排除重複、超過四位及非數字，支援 LF 與 CRLF。 */
static void Process_Byte(uint8_t byte)
{
  if ((byte == '\r') || (byte == '\n'))
  {
    if ((byte == '\n') && (last_was_cr != 0U)) /* CRLF 只計算一次換行。 */
    {
      last_was_cr = 0U;
      return;
    }
    last_was_cr = (uint8_t)(byte == '\r');
    Process_Line();
    input_length = 0U;
    input_invalid = 0U;
    return;
  }
  last_was_cr = 0U;
  if ((byte < '0') || (byte > '9') || (input_length >= GAME_LEN))
  {
    input_invalid = 1U;
    return;
  }
  for (uint8_t i = 0U; i < input_length; ++i)
  {
    if (input_digits[i] == (uint8_t)(byte - '0')) input_invalid = 1U;
  }
  input_digits[input_length++] = (uint8_t)(byte - '0'); /* ASCII 轉為 0~9 數值。 */
}
/* USER CODE END 0 */
/* 主程式進入點。 */
int main(void)
{
  /* USER CODE BEGIN 1 */
  /* USER CODE END 1 */
  /* 微控制器基本初始化 */
  /* 重設周邊並初始化 Flash 與 SysTick（提供 HAL_GetTick 毫秒計時）。 */
  HAL_Init();
  /* USER CODE BEGIN Init */
  /* USER CODE END Init */
  /* 設定系統時脈（依 CubeMX 產生的 170 MHz 設定）。 */
  SystemClock_Config();
  /* USER CODE BEGIN SysInit */
  /* USER CODE END SysInit */
  /* 初始化 GPIO、LPUART1 與 TIM3 等周邊。 */
  MX_GPIO_Init();
  MX_LPUART1_UART_Init();
  MX_TIM3_Init();
  /* USER CODE BEGIN 2 */
/* 啟用 DWT CPU 週期計數器，以玩家第一次操作的時間作亂數種子。
 * 啟動 TIM3 CH1~CH4 PWM，然後先讓四顆 LED 熄滅。
 */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  for (uint8_t i = 0U; i < GAME_LEN; ++i)
  {
    if (HAL_TIM_PWM_Start(&htim3, led_channels[i]) != HAL_OK) Error_Handler();
  }
  LEDs_Clear();
  /* USER CODE END 2 */
  /* 主迴圈持續處理重新開局、LED 呼吸及 UART 輸入。 */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */
    /* USER CODE BEGIN 3 */
/* 主迴圈：先處理按鈕設定的重開旗標，再更新呼吸燈，最後輪詢 UART。
 * 中斷內不做 UART 傳送及亂數處理，避免中斷執行時間過長。
 */
    uint8_t byte;
    /* PA0 中斷若要求重新開始，主迴圈在此處理。 */
  if (restart_requested != 0U)
    {
      uint32_t action_tick;
      uint32_t action_cycle;
      __disable_irq();
      restart_requested = 0U;
      action_tick = first_button_tick;
      action_cycle = first_button_cycle;
      __enable_irq();
      if (rng_seeded == 0U) Seed_RNG(action_tick, action_cycle);
      Start_New_Game(1U);
    }
    Breath_Update();
    /* 每次輪詢 1 byte，最多等待 1 ms（短暫阻塞）。 */
  if (HAL_UART_Receive(&hlpuart1, &byte, 1U, RX_TIMEOUT_MS) == HAL_OK)
    {
      /* 第一次收到字元時才啟動第一局並以目前操作時間作種子。 */
    if (game_started == 0U)
      {
        Seed_RNG(HAL_GetTick(), DWT->CYCCNT);
        Start_New_Game(0U);
      }
      Process_Byte(byte);
    }
  }
  /* USER CODE END 3 */
}
/* 設定系統時脈為 CubeMX 產生的設定。 */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);
  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV4;
  RCC_OscInitStruct.PLL.PLLN = 85;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}
/* 初始化 LPUART1：115200、8N1。 */
static void MX_LPUART1_UART_Init(void)
{
  /* USER CODE BEGIN LPUART1_Init 0 */
  /* USER CODE END LPUART1_Init 0 */
  /* USER CODE BEGIN LPUART1_Init 1 */
  /* USER CODE END LPUART1_Init 1 */
  hlpuart1.Instance = LPUART1;
  hlpuart1.Init.BaudRate = 115200;
  hlpuart1.Init.WordLength = UART_WORDLENGTH_8B;
  hlpuart1.Init.StopBits = UART_STOPBITS_1;
  hlpuart1.Init.Parity = UART_PARITY_NONE;
  hlpuart1.Init.Mode = UART_MODE_TX_RX;
  hlpuart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  hlpuart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  hlpuart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  hlpuart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&hlpuart1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&hlpuart1, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&hlpuart1, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&hlpuart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN LPUART1_Init 2 */
  /* USER CODE END LPUART1_Init 2 */
}
/* 初始化 TIM3 四通道 PWM，預分頻 169，週期 999。 */
static void MX_TIM3_Init(void)
{
  /* USER CODE BEGIN TIM3_Init 0 */
  /* USER CODE END TIM3_Init 0 */
  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};
  /* USER CODE BEGIN TIM3_Init 1 */
  /* USER CODE END TIM3_Init 1 */
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 169;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 999;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim3, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_2) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_3) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_4) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM3_Init 2 */
  /* USER CODE END TIM3_Init 2 */
  HAL_TIM_MspPostInit(&htim3);
}
/* 初始化 GPIO 與 PA0 外部中斷。 */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */
  /* USER CODE END MX_GPIO_Init_1 */
  /* 啟用 GPIO 埠時鐘，才能設定輸入／輸出腳位。 */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  /* 設定 PA0 重新開始按鈕為下降沿中斷，啟用內部上拉 */
  GPIO_InitStruct.Pin = RESTART_BTN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(RESTART_BTN_GPIO_Port, &GPIO_InitStruct);
  /* 啟用 PA0 對應的 EXTI0 外部中斷。 */
  HAL_NVIC_SetPriority(EXTI0_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI0_IRQn);
  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* USER CODE END MX_GPIO_Init_2 */
}
/* USER CODE BEGIN 4 */
/* PA0 下降沿中斷的 HAL callback。
 * 只判斷防彈跳、記錄按下時刻、設定 restart_requested。
 * 真正公布答案／開始新局由 main() 主迴圈處理。
 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == RESTART_BTN_Pin)
  {
    uint32_t now = HAL_GetTick();
    /* 首次按下直接接受，其餘間隔至少 200 毫秒。 */
  if ((button_seen == 0U) || ((uint32_t)(now - last_button_tick) >= DEBOUNCE_MS))
    {
      button_seen = 1U;
      last_button_tick = now;
      /* 主迴圈尚未處理前，不覆寫第一次按下時間。 */
      if (restart_requested == 0U)
      {
        first_button_tick = now;
        first_button_cycle = DWT->CYCCNT;
        restart_requested = 1U;
      } /* 中斷只通知主迴圈，不在此重新開局。 */
    }
  }
}
/* USER CODE END 4 */
/* 初始化或操作失敗時停留於此函式。 */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* HAL 發生錯誤時停在此處，方便使用除錯器追查。 */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/* 斷言失敗時可使用檔名與行號追查錯誤。 */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* 需要時可在此輸出斷言失敗的檔名與行號，
     例如透過 UART 輸出錯誤位置。 */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
