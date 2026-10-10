// SPDX-License-Identifier: GPL-2.0-only
// bbmp-tiatracker: the few parts of Qt that TIATracker's song and replay code uses,
// made small with the C++ standard library, so the tracker's own files build without
// Qt. Written for bbmp; not Qt's code. Only what those files call is here.
//
// QList's [] checks its index and throws std::out_of_range: a song that would make the
// tracker's code read outside a list ends with an error instead.
#ifndef BBMP_QTSHIM_H
#define BBMP_QTSHIM_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// ---- Qt's macros for signals and slots: plain member functions here ----
#define Q_OBJECT
#define signals public
#define slots
#define emit

template <typename T> inline const T &qMin(const T &a, const T &b) { return b < a ? b : a; }
template <typename T> inline const T &qMax(const T &a, const T &b) { return a < b ? b : a; }

// ---- QString: UTF-8 text ----
class QString {
public:
    QString() {}
    QString(const char *s) : s_(s ? s : "") {}
    QString(std::string s) : s_(std::move(s)) {}

    // As Qt counts: in UTF-16 units
    int length() const {
        int n = 0;
        for (unsigned char c : s_) {
            if ((c & 0xc0) != 0x80) n += (c >= 0xf0) ? 2 : 1;
        }
        return n;
    }
    int size() const { return length(); }
    bool isEmpty() const { return s_.empty(); }
    bool empty() const { return s_.empty(); }
    const std::string &str() const { return s_; }
    const char *c_str() const { return s_.c_str(); }

    // The lowest %n in the text is replaced, as Qt's arg does
    QString arg(int v) const {
        int lowest = 100;
        for (size_t i = 0; i + 1 < s_.size(); ++i)
            if (s_[i] == '%' && s_[i + 1] >= '1' && s_[i + 1] <= '9')
                lowest = std::min(lowest, s_[i + 1] - '0');
        if (lowest == 100) return *this;
        std::string out, num = std::to_string(v);
        for (size_t i = 0; i < s_.size(); ++i) {
            if (s_[i] == '%' && i + 1 < s_.size() && s_[i + 1] - '0' == lowest) {
                out += num;
                ++i;
            } else {
                out += s_[i];
            }
        }
        return QString(out);
    }
    static QString number(int v) { return QString(std::to_string(v)); }

    QString &operator+=(const QString &o) { s_ += o.s_; return *this; }
    friend QString operator+(const QString &a, const QString &b) { return QString(a.s_ + b.s_); }
    friend QString operator+(const char *a, const QString &b) { return QString(std::string(a) + b.s_); }
    friend QString operator+(const QString &a, const char *b) { return QString(a.s_ + b); }
    friend bool operator==(const QString &a, const QString &b) { return a.s_ == b.s_; }
    friend bool operator!=(const QString &a, const QString &b) { return a.s_ != b.s_; }
    friend bool operator==(const QString &a, const char *b) { return a.s_ == b; }
    friend bool operator!=(const QString &a, const char *b) { return a.s_ != b; }
    friend bool operator<(const QString &a, const QString &b) { return a.s_ < b.s_; }

private:
    std::string s_;
};

// ---- QList: a vector whose [] checks the index ----
static const char *const OUTSIDE = "a number in it points outside its list";
template <typename T>
class QList {
public:
    QList() {}
    QList(std::initializer_list<T> l) : v_(l) {}

    int size() const { return int(v_.size()); }
    int count() const { return size(); }
    bool isEmpty() const { return v_.empty(); }
    T &operator[](int i) { return v_[check(i)]; }
    const T &operator[](int i) const { return v_[check(i)]; }
    T &last() { return v_[check(size() - 1)]; }
    const T &last() const { return v_[check(size() - 1)]; }
    void append(const T &t) { v_.push_back(t); }
    void clear() { v_.clear(); }
    void removeLast() { if (v_.empty()) throw std::out_of_range(OUTSIDE); v_.pop_back(); }
    void removeAt(int i) { v_.erase(v_.begin() + check(i)); }
    void insert(int i, const T &t) {
        if (i < 0 || i > size()) throw std::out_of_range(OUTSIDE);
        v_.insert(v_.begin() + i, t);
    }
    int indexOf(const T &t) const {
        for (int i = 0; i < size(); ++i)
            if (v_[size_t(i)] == t) return i;
        return -1;
    }
    typename std::vector<T>::iterator begin() { return v_.begin(); }
    typename std::vector<T>::iterator end() { return v_.end(); }
    typename std::vector<T>::const_iterator begin() const { return v_.begin(); }
    typename std::vector<T>::const_iterator end() const { return v_.end(); }

private:
    size_t check(int i) const {
        if (i < 0 || i >= size()) throw std::out_of_range(OUTSIDE);
        return size_t(i);
    }
    std::vector<T> v_;
};

template <typename T> using QVector = QList<T>;

// ---- QMap and its Java-style iterator ----
template <typename K, typename V>
class QMap {
public:
    QMap() {}
    V &operator[](const K &k) { return m_[k]; }
    int size() const { return int(m_.size()); }
    const std::map<K, V> &map() const { return m_; }

private:
    std::map<K, V> m_;
};

template <typename K, typename V>
class QMapIterator {
public:
    explicit QMapIterator(const QMap<K, V> &m) : m_(&m.map()), next_(m_->begin()) {}
    bool hasNext() const { return next_ != m_->end(); }
    void next() { cur_ = next_++; }
    const K &key() const { return cur_->first; }
    const V &value() const { return cur_->second; }

private:
    const std::map<K, V> *m_;
    typename std::map<K, V>::const_iterator next_, cur_;
};

// ---- QMutex: one thread here, so nothing to lock ----
class QMutex {
public:
    void lock() {}
    void unlock() {}
};

// ---- QObject and the rest of the event loop the replay code mentions ----
class QObject {
public:
    explicit QObject(QObject * = nullptr) {}
    virtual ~QObject() {}
};

class QElapsedTimer {
public:
    void start() {}
    long long nsecsElapsed() const { return 0; }
    long long elapsed() const { return 0; }
    void restart() {}
};

struct QEventLoop { enum { AllEvents = 0 }; };
struct QCoreApplication { static void processEvents(int, int) {} };
struct QThread { static void msleep(unsigned long) {} };
class QTimer {};

// ---- QJsonValue, QJsonArray, QJsonObject ----
class QJsonArray;
class QJsonObject;

class QJsonValue {
public:
    enum Type { Null, Bool, Double, String, Array, Object, Undefined };

    QJsonValue(Type t = Null) : t_(t) {}
    QJsonValue(bool b) : t_(Bool), b_(b) {}
    QJsonValue(int v) : t_(Double), d_(v) {}
    QJsonValue(double v) : t_(Double), d_(v) {}
    QJsonValue(const char *s) : t_(String), s_(s) {}
    QJsonValue(const QString &s) : t_(String), s_(s) {}
    QJsonValue(const QJsonArray &a);
    QJsonValue(const QJsonObject &o);

    Type type() const { return t_; }
    bool isUndefined() const { return t_ == Undefined; }

    // As Qt 5 does: a number that is a whole int, otherwise the default
    int toInt(int def = 0) const {
        if (t_ == Double && std::isfinite(d_) && d_ >= -2147483648.0 && d_ <= 2147483647.0
                && double(int(d_)) == d_)
            return int(d_);
        return def;
    }
    bool toBool(bool def = false) const { return t_ == Bool ? b_ : def; }
    double toDouble(double def = 0.0) const { return t_ == Double ? d_ : def; }
    QString toString() const { return t_ == String ? s_ : QString(); }
    QJsonArray toArray() const;
    QJsonObject toObject() const;

    friend bool operator==(const QJsonValue &v, const char *s) { return v.t_ == String && v.s_ == s; }
    friend bool operator!=(const QJsonValue &v, const char *s) { return !(v == s); }

private:
    Type t_;
    bool b_ = false;
    double d_ = 0.0;
    QString s_;
    std::shared_ptr<const QJsonArray> a_;
    std::shared_ptr<const QJsonObject> o_;
};

class QJsonArray {
public:
    int size() const { return int(v_.size()); }
    int count() const { return size(); }
    // Outside the array: undefined, as Qt's at() gives
    QJsonValue operator[](int i) const {
        if (i < 0 || i >= size()) return QJsonValue(QJsonValue::Undefined);
        return v_[size_t(i)];
    }
    QJsonValue at(int i) const { return (*this)[i]; }
    void append(const QJsonValue &v) { v_.push_back(v); }

private:
    std::vector<QJsonValue> v_;
};

class QJsonObject {
public:
    // Missing: undefined, as Qt gives
    QJsonValue operator[](const QString &k) const {
        auto it = m_.find(k.str());
        return it == m_.end() ? QJsonValue(QJsonValue::Undefined) : it->second;
    }
    QJsonValue value(const QString &k) const { return (*this)[k]; }
    QJsonValue &operator[](const QString &k) { return m_[k.str()]; }
    bool contains(const QString &k) const { return m_.count(k.str()) != 0; }
    void insert(const QString &k, const QJsonValue &v) { m_[k.str()] = v; }
    int size() const { return int(m_.size()); }

private:
    std::map<std::string, QJsonValue> m_;
};

inline QJsonValue::QJsonValue(const QJsonArray &a)
    : t_(Array), a_(std::make_shared<const QJsonArray>(a)) {}
inline QJsonValue::QJsonValue(const QJsonObject &o)
    : t_(Object), o_(std::make_shared<const QJsonObject>(o)) {}
inline QJsonArray QJsonValue::toArray() const { return t_ == Array && a_ ? *a_ : QJsonArray(); }
inline QJsonObject QJsonValue::toObject() const { return t_ == Object && o_ ? *o_ : QJsonObject(); }

// A small JSON reader (RFC 8259) for the song file. Fills `out` with the top object;
// on failure returns false and says why in `why`. Nesting deeper than 64 is refused.
bool bbmpParseJson(const std::string &text, QJsonObject &out, std::string &why);

#endif
