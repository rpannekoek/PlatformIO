class VoltageSensor
{
    public:
        VoltageSensor(uint8_t pin);

        bool begin();

        bool detectSignal(uint32_t sensePeriodMs = 100);
        void setTestState(bool signalDetected);

    private:
        uint8_t _pin;
        bool _testState;
        bool _testMode = false;
};