#ifndef MOVING_AVERAGE_H
#define MOVING_AVERAGE_H

#include <deque>

class MovingAverage
{
    public:
        // Constructor
        MovingAverage(int length)
        {
            _length = length;
        }

        void addValue(int value)
        {
            _aggregate += value;
            _history.push_back(value);
            if (_history.size() > _length)
            {
                _aggregate -= _history.front();
                _history.pop_front();
            }
        }

        float getAverage() const 
        { 
            return (_history.size() == 0) ? 0.0F : float(_aggregate) / _history.size();
        }

        float getSlope() const
        {
            if (_history.size() == 0) return 0;
            float delta = _history.back() - _history.front();
            return delta / _history.size();
        }

        int getMinimum() const
        {
            if (_history.size() == 0) return 0;
            int result = _history.front();
            for (int value : _history)
                result = std::min(result, value);
            return result;
        }

        int getMaximum() const
        {
            if (_history.size() == 0) return 0;
            int result = _history.front();
            for (int value : _history)
                result = std::max(result, value);
            return result;
        }

    private:
        int _length;
        std::deque<int> _history;
        int _aggregate = 0;
};

#endif