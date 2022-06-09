#include <iostream>
#include <queue>
#include <mutex>
#include <future>
#include <functional>

template <typename T>
class SafeQueue
{
private:
    std::queue<T> m_queue; // 利用模板函数构造队列
    std::mutex m_mutex;    // 访问互斥信号量

public:
    SafeQueue() {}
    SafeQueue(SafeQueue &&other) {}
    ~SafeQueue() {}

    bool empty()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_queue.empty();
    }

    int size()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_queue.size();
    }

    void enqueue(const T &t)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_queue.emplace(t);
    }

    bool dequeue(T &t)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if(m_queue.empty()){
            return false;
        }
        t = std::move(m_queue.front());
        m_queue.pop();
        return true;
    }
};

class ThreadPool
{
private:
    bool m_shutdown;                            // 线程池是否关闭
    SafeQueue<std::function<void()>> m_queue;   // 执行函数安全队列，即任务队列
    std::vector<std::thread> m_threads;         // 工作线程队列
    std::mutex m_conditional_mutex;             // 线程休眠锁互斥变量
    std::condition_variable m_conditional_lock; // 线程环境锁，可以让线程处于休眠或者唤醒状态

    class ThreadWorker
    {
    private:
        int m_id;           // 工作id
        ThreadPool *m_pool; // 所属线程池
    public:
        ThreadWorker(ThreadPool *pool, const int id) : m_pool(pool), m_id(id)
        {
        }
        void operator()()
        {
            std::function<void()> func; // 定义基础函数类func
            bool dequeued;              // 是否成功取出队列中的元素
            while (!m_pool->m_shutdown)
            {
                {
                    // 为线程环境加锁，互访问工作线程的休眠和唤醒
                    std::unique_lock<std::mutex> lock(m_pool->m_conditional_mutex);

                    // 如果任务队列为空，阻塞当前线程
                    if(m_pool->m_queue.empty())
                    {
                        m_pool->m_conditional_lock.wait(lock);  // 等待条件变量通知，开启线程
                    }

                    // 取出任务队列中的元素
                    dequeued = m_pool->m_queue.dequeue(func);
                }

                // 如果成功取出，则执行工作函数
                if(dequeued){
                    func();
                }
            }
        }
    };

public:
    ThreadPool(const int n_threads = 4) : m_threads(std::vector<std::thread>(n_threads)), m_shutdown(false)
    {
    }
    ThreadPool(const ThreadPool &) = delete;
    ThreadPool(ThreadPool &&) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;
    ThreadPool &operator=(ThreadPool &&) = delete;

    void init()
    {
        for (int i = 0; i < m_threads.size(); ++i)
        {
            m_threads.at(i) = std::thread(ThreadWorker(this, i));
        }
    }

    void shutdown()
    {
        m_shutdown = true;
        m_conditional_lock.notify_all();
        for (int i = 0; i < m_threads.size(); ++i)
        {
            if (m_threads.at(i).joinable())
            {
                m_threads.at(i).join();
            }
        }
    }

    template <typename F, typename... Args>
    auto submit(F &&f, Args &&...args) -> std::future<decltype(f(args...))>
    {
        // 创造一个函数，使用bind绑定了函数和参数，并存放在function中
        std::function<decltype(f(args...))> func = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
        // 封装到共享指针
        auto task_ptr = std::make_shared<std::packaged_task<decltype(f(args...))()>>(func);
        // 将打包的任务变形为void函数
        std::function<void()> warpper_func = [task_ptr]()
        {
            (*task_ptr)();
        };
        // 队列通用安全封包函数，并压入安全队列
        m_queue.enqueue(warpper_func);
        // 唤醒一个等待中的线程
        m_conditional_lock.notify_one();
        // 返回先前注册的任务指针
        return task_ptr->get_future();
    }
};