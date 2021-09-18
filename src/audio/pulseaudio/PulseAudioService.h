#ifndef PULSEAUDIOSERVICE_H
#define PULSEAUDIOSERVICE_H

#include <memory>

#include "IAudioService.h"

class PulseAudioProcessingThread;

class PulsePipelineManager;
typedef std::shared_ptr<PulsePipelineManager> PulsePipelineManagerPtr;

class PulseAudioService : public IAudioService
{
    Q_OBJECT
    Q_INTERFACES(IAudioService)

public:
    PulseAudioService();
    ~PulseAudioService();

public slots:
    void update(DspConfig* config) override;
    void reloadLiveprog() override;
    void reloadService() override;

    IAppManager* appManager() override;
    std::vector<IOutputDevice> sinkDevices() override;
    DspStatus status() override;

private:
    PulsePipelineManagerPtr mgr;
    PulseAudioProcessingThread* apt;

};

#endif // PULSEAUDIOSERVICE_H