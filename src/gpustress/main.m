// GPU Stress: a steady load on the GPU through Core Animation (the same path as the desktop), at a level the user picks.
// The animations run in the render server, not in this app, so the load is the GPU's and the render server's, and the
// same from run to run. Every 5 s the app says what it measured in the log ("gpustress:").
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>

@interface StressController : UIViewController
@end

@implementation StressController {
    UIView *_stage;
    UISegmentedControl *_levels;
    UILabel *_info;
    CADisplayLink *_link;
    CFTimeInterval _start, _windowStart, _last, _worst;
    NSInteger _frames, _late, _level;
}

static const NSInteger kLayers[] = {0, 40, 200, 600};
static const NSInteger kBlurs[] = {0, 0, 2, 6};

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    _stage = [[UIView alloc] initWithFrame:self.view.bounds];
    _stage.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:_stage];

    _levels = [[UISegmentedControl alloc] initWithItems:@[ @"Выкл", @"Лёгкая", @"Средняя", @"Тяжёлая" ]];
    _levels.selectedSegmentIndex = 0;
    _levels.backgroundColor = [UIColor colorWithWhite:0.15 alpha:1];
    [_levels addTarget:self action:@selector(levelChanged) forControlEvents:UIControlEventValueChanged];
    [self.view addSubview:_levels];

    _info = [[UILabel alloc] init];
    _info.numberOfLines = 0;
    _info.textColor = UIColor.whiteColor;
    _info.font = [UIFont monospacedDigitSystemFontOfSize:17 weight:UIFontWeightSemibold];
    _info.backgroundColor = [UIColor colorWithWhite:0 alpha:0.6];
    _info.text = @"Выбери нагрузку";
    [self.view addSubview:_info];

    _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
    if (@available(iOS 15.0, *)) _link.preferredFrameRateRange = CAFrameRateRangeMake(120, 120, 120);
    [_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    CGRect b = self.view.bounds;
    CGFloat top = self.view.safeAreaInsets.top + 8;
    _levels.frame = CGRectMake(16, top, b.size.width - 32, 36);
    _info.frame = CGRectMake(16, top + 44, b.size.width - 32, 110);
}

- (void)levelChanged {
    _level = _levels.selectedSegmentIndex;
    for (UIView *v in [_stage.subviews copy]) [v removeFromSuperview];
    for (CALayer *l in [_stage.layer.sublayers copy]) [l removeFromSuperlayer];
    CGSize s = _stage.bounds.size;
    NSInteger n = kLayers[_level];
    srand48(42);
    for (NSInteger i = 0; i < n; i++) {
        CALayer *l = [CALayer layer];
        CGFloat side = 60 + drand48() * 160;
        l.bounds = CGRectMake(0, 0, side, side);
        l.position = CGPointMake(drand48() * s.width, drand48() * s.height);
        l.cornerRadius = side * 0.25;
        l.backgroundColor = [UIColor colorWithHue:drand48() saturation:0.8 brightness:0.9 alpha:0.35].CGColor;
        CABasicAnimation *spin = [CABasicAnimation animationWithKeyPath:@"transform.rotation.z"];
        spin.fromValue = @0;
        spin.toValue = @(2 * M_PI * (drand48() < 0.5 ? 1 : -1));
        spin.duration = 2 + drand48() * 4;
        spin.repeatCount = HUGE_VALF;
        [l addAnimation:spin forKey:@"spin"];
        CABasicAnimation *move = [CABasicAnimation animationWithKeyPath:@"position"];
        move.toValue = [NSValue valueWithCGPoint:CGPointMake(drand48() * s.width, drand48() * s.height)];
        move.duration = 3 + drand48() * 5;
        move.autoreverses = YES;
        move.repeatCount = HUGE_VALF;
        [l addAnimation:move forKey:@"move"];
        [_stage.layer addSublayer:l];
    }
    for (NSInteger i = 0; i < kBlurs[_level]; i++) {
        UIVisualEffectView *blur = [[UIVisualEffectView alloc] initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemThinMaterialDark]];
        CGFloat h = s.height / 3;
        blur.frame = CGRectMake(20, 180 + (i % 3) * (h - 40), s.width - 40, h - 60);
        blur.layer.cornerRadius = 30;
        blur.clipsToBounds = YES;
        [_stage addSubview:blur];
        [UIView animateWithDuration:2.5 + i * 0.4 delay:0 options:UIViewAnimationOptionRepeat | UIViewAnimationOptionAutoreverse | UIViewAnimationOptionCurveEaseInOut
                         animations:^{ blur.transform = CGAffineTransformMakeTranslation(0, (i % 2 ? -1 : 1) * 120); }
                         completion:nil];
    }
    _start = _windowStart = CACurrentMediaTime();
    _frames = _late = 0;
    _worst = 0;
    NSLog(@"gpustress: level %ld: %ld layers, %ld blurs", (long)_level, (long)n, (long)kBlurs[_level]);
    printf("gpustress: level %ld: %ld layers, %ld blurs\n", (long)_level, (long)n, (long)kBlurs[_level]);
}

- (void)tick:(CADisplayLink *)link {
    CFTimeInterval now = link.timestamp;
    if (_last > 0) {
        CFTimeInterval dt = now - _last;
        _worst = MAX(_worst, dt);
        if (dt > 1.5 / 120) _late++;
    }
    _last = now;
    _frames++;
    if (now - _windowStart >= 5) {
        double fps = _frames / (now - _windowStart);
        long secs = _start > 0 ? (long)(now - _start) : 0;
        _info.text = [NSString stringWithFormat:@" Нагрузка: %@\n Время: %ld:%02ld\n %.0f кадров/с, худший кадр %.1f мс\n пропусков за 5 с: %ld",
                      [_levels titleForSegmentAtIndex:_level], secs / 60, secs % 60, fps, _worst * 1000, (long)_late];
        NSLog(@"gpustress: level %ld, %ld s: %.1f fps, worst %.1f ms, %ld late", (long)_level, secs, fps, _worst * 1000, (long)_late);
        printf("gpustress: level %ld, %ld s: %.1f fps, worst %.1f ms, %ld late\n", (long)_level, secs, fps, _worst * 1000, (long)_late);
        _windowStart = now;
        _frames = _late = 0;
        _worst = 0;
    }
}

- (BOOL)prefersStatusBarHidden { return YES; }
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow *window;
@end

@implementation AppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)options {
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [StressController new];
    [self.window makeKeyAndVisible];
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    return YES;
}
@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}
