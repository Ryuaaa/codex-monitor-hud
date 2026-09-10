#import <Foundation/Foundation.h>
NSString *HUDL(NSString *key);
NSString *HUDLanguage(void);
NSString *HUDCurrency(void);
NSString *HUDMoney(double usd);
NSString *HUDExchangeRateDate(void);
void HUDRefreshExchangeRates(void);
