#include "providers/twitch/ModeratorExtension.hpp"
#include "providers/twitch/AutomationDialog.hpp"
#include "Application.hpp"
#include "common/Args.hpp"
#include "common/UiLanguage.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Theme.hpp"
#include "widgets/dialogs/MoltorinoDialogTheme.hpp"

#include <QCryptographicHash>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QQueue>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <openssl/evp.h>
#include <algorithm>

namespace chatterino::moderatorextension {
namespace {
constexpr qsizetype MAX_BUNDLE=128*1024;
constexpr auto MODULE_URL="https://raw.githubusercontent.com/Mist10X/Chatterino-Noctis/main/moderator/extension.json";

QByteArray boundedRead(const QString &path)
{
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly) || file.size()>MAX_BUNDLE) return {};
    return file.read(MAX_BUNDLE+1);
}
bool signatureOk(const QByteArray &key,const QByteArray &data,const QByteArray &signature)
{
    if(key.size()!=32 || signature.size()!=64)return false;
    using Key=std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)>;
    using Ctx=std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)>;
    Key p(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519,nullptr,
        reinterpret_cast<const unsigned char *>(key.constData()),key.size()),EVP_PKEY_free);
    Ctx c(EVP_MD_CTX_new(),EVP_MD_CTX_free);
    return p && c && EVP_DigestVerifyInit(c.get(),nullptr,nullptr,nullptr,p.get())==1 &&
        EVP_DigestVerify(c.get(),reinterpret_cast<const unsigned char *>(signature.constData()),signature.size(),
            reinterpret_cast<const unsigned char *>(data.constData()),data.size())==1;
}
std::optional<Text> text(const QJsonValue &v,int limit)
{
    if(!v.isObject())return {};
    const auto obj=v.toObject();
    Text t{obj.value("en").toString(),obj.value("ru").toString()};
    if(t.en.trimmed().isEmpty() || t.ru.trimmed().isEmpty() ||
        t.en.size()>limit || t.ru.size()>limit)return {};
    return t;
}
bool onlyKeys(const QJsonObject &obj,const QSet<QString> &keys)
{
    for(auto it=obj.begin();it!=obj.end();++it)if(!keys.contains(it.key()))return false;
    return true;
}
bool integer(const QJsonValue &v)
{
    return v.isDouble() && v.toDouble()==v.toInt() && v.toInt()>=-1000000 && v.toInt()<=1000000;
}
bool fieldValue(const Field &f,const QJsonValue &v)
{
    if(f.type=="integer")return integer(v) && v.toInt()>=f.minimum && v.toInt()<=f.maximum;
    if(f.type=="boolean")return v.isBool();
    if(f.type=="text")return v.isString() && v.toString().size()<=f.maximum;
    if(f.type=="choice")
        for(const auto &c:f.choices)if(v.isString() && c.id==v.toString())return true;
    return false;
}
const QSet<QString> eventKeys{"source","message","target","channel","buyer"};
bool checkExpr(const QJsonValue &expr,const QSet<QString> &fields,int depth,int &budget)
{
    if(depth>8 || --budget<0)return false;
    if(expr.isBool() || integer(expr))return true;
    if(expr.isString())return expr.toString().size()<=500;
    const auto obj=expr.toObject();
    if(obj.size()!=1)return false;
    const auto op=obj.begin().key();const auto value=obj.begin().value();
    if(op=="param" || op=="event")
        return value.isString() && (op=="param"?fields:eventKeys).contains(value.toString());
    if(!value.isArray())return false;
    const auto args=value.toArray();
    if(op=="if" ? args.size()!=3 : op=="not" ? args.size()!=1 :
       op=="concat" ? (args.isEmpty() || args.size()>8) :
       (op=="eq" || op=="less" || op=="contains" || op=="random" || op=="add" ||
        op=="min" || op=="max" || op=="and" || op=="or") ? args.size()!=2 : true)return false;
    for(auto arg:args)if(!checkExpr(arg,fields,depth+1,budget))return false;
    return true;
}
std::optional<QJsonValue> eval(const QJsonValue &expr,const QJsonObject &params,
                               const QJsonObject &ctx,int depth,int &budget)
{
    if(depth>8 || --budget<0)return {};
    if(expr.isBool() || integer(expr) || (expr.isString() && expr.toString().size()<=500))return expr;
    const auto obj=expr.toObject();
    if(obj.size()!=1)return {};
    const auto op=obj.begin().key();const auto value=obj.begin().value();
    if(op=="param" || op=="event")
    {
        const auto result=(op=="param"?params:ctx).value(value.toString());
        if(result.isUndefined() || result.isNull())return {};
        return result;
    }
    const auto args=value.toArray();
    const auto arg=[&](int i)->std::optional<QJsonValue>{
        if(i<0 || i>=args.size())return {};
        return eval(args.at(i),params,ctx,depth+1,budget);
    };
    if(op=="if")
    {
        const auto condition=arg(0);
        return condition && condition->isBool()?arg(condition->toBool()?1:2):std::nullopt;
    }
    if(op=="concat")
    {
        QString out;
        for(int i=0;i<args.size();++i)
        {
            const auto v=arg(i);
            if(!v || (!v->isString() && !integer(*v)))return {};
            out+=v->isString()?v->toString():QString::number(v->toInt());
            if(out.size()>500)return {};
        }
        return QJsonValue(out);
    }
    const auto a=arg(0);
    if(!a)return {};
    if(op=="not")return a->isBool()?std::optional<QJsonValue>(QJsonValue(!a->toBool())):std::nullopt;
    const auto b=arg(1);
    if(!b)return {};
    if(op=="eq")return QJsonValue(*a==*b);
    if(op=="contains")
        return a->isString()&&b->isString()?std::optional<QJsonValue>(QJsonValue(a->toString().contains(b->toString(),Qt::CaseInsensitive))):std::nullopt;
    if(op=="and" || op=="or")
        return a->isBool()&&b->isBool()?std::optional<QJsonValue>(QJsonValue(op=="and"?a->toBool()&&b->toBool():a->toBool()||b->toBool())):std::nullopt;
    if(!integer(*a)||!integer(*b))return {};
    const int x=a->toInt(),y=b->toInt();
    if(op=="less")return QJsonValue(x<y);
    if(op=="min")return QJsonValue(std::min(x,y));
    if(op=="max")return QJsonValue(std::max(x,y));
    if(op=="add" && x+y>=-1000000 && x+y<=1000000)return QJsonValue(x+y);
    if(op=="random" && x>=0 && y>=x && y<=86400)
        return QJsonValue(QRandomGenerator::global()->bounded(x,y+1));
    return {};
}
std::optional<Field> parseField(const QJsonObject &o)
{
    if(!onlyKeys(o,{"id","type","title","default","minimum","maximum","choices"}))return {};
    Field f;f.id=o.value("id").toString();f.type=o.value("type").toString();
    auto title=text(o.value("title"),64);
    if(!validActionId(f.id)||!title)return {};
    f.title=*title;f.initial=o.value("default");f.minimum=o.value("minimum").toInt(0);
    f.maximum=o.value("maximum").toInt(0);
    if(f.type=="integer")
    {
        if(!integer(o.value("minimum")) || !integer(o.value("maximum")) ||
           f.minimum<0 || f.maximum<f.minimum || f.maximum>86400)return {};
    }
    else if(f.type=="text")
    {
        if(!integer(o.value("maximum")) || f.maximum<1 || f.maximum>500)return {};
    }
    else if(f.type=="choice")
    {
        const auto choices=o.value("choices").toArray();
        if(choices.isEmpty() || choices.size()>12)return {};
        QSet<QString> ids;
        for(const auto v:choices)
        {
            const auto c=v.toObject();if(!onlyKeys(c,{"id","title"}))return {};
            const auto id=c.value("id").toString();const auto label=text(c.value("title"),64);
            if(!validActionId(id)||ids.contains(id)||!label)return {};
            ids.insert(id);f.choices.append({id,*label});
        }
    }
    else if(f.type!="boolean")return {};
    return fieldValue(f,f.initial)?std::optional<Field>(std::move(f)):std::nullopt;
}
std::optional<QStringList> parseOrder(const QJsonValue &v,const QStringList &defaults)
{
    if(!v.isArray() || v.toArray().size()!=defaults.size())return {};
    QStringList order;
    for(const auto item:v.toArray())
    {
        const auto id=item.toString();
        if(!defaults.contains(id)||order.contains(id))return {};
        order.append(id);
    }
    return order;
}
struct Runtime : QObject {
    explicit Runtime(const QString &directory)
        : storage(directory.isEmpty()?QString{}:QDir(directory).filePath("noctis-moderator-extension.json"),ownerKey())
    {
        timer.setInterval(5*60*1000);
        QObject::connect(&timer,&QTimer::timeout,this,[this]{refresh();});
    }
    Store storage;
    QTimer timer;
    bool fetching=false;
    QString status;
    void refresh()
    {
        if(fetching)return;
        if(getApp()->isTest() || getApp()->getArgs().safeMode ||
           getApp()->getArgs().dontSaveSettings || getApp()->getArgs().uiSnapshot)return;
        fetching=true;
        status=uiText("Checking module updates...");
        NetworkRequest(QUrl(QString::fromLatin1(MODULE_URL))).caller(this)
            .timeout(12000).followRedirects(false)
            .header("Cache-Control","no-cache")
            .onSuccess([this](const NetworkResult &result){
                fetching=false;
                setStatus(storage.offer(result.getData()));
            })
            .onError([this](const NetworkResult &){
                fetching=false;
                status=uiText("No compatible module update available.");
            }).execute();
    }
    void setStatus(Store::Result result)
    {
        switch(result)
        {
            case Store::Result::Activated: status=uiText("Module updated.");break;
            case Store::Result::Deferred: status=uiText("Update verified. Close the settings windows to apply it.");break;
            case Store::Result::Unchanged: status=uiText("Module is up to date.");break;
            case Store::Result::Rejected: status=uiText("Module package rejected; the current version is unchanged.");break;
        }
    }
};
Runtime &runtime()
{
    // Tests never load profile/cache files or contact GitHub.
    static Runtime fixture(QString{});
    if(getApp()->isTest() || getApp()->getArgs().safeMode || getApp()->getArgs().dontSaveSettings || getApp()->getArgs().uiSnapshot)
        return fixture;
    static auto *instance=new Runtime(getApp()->getPaths().settingsDirectory);
    return *instance;
}
void openModuleDialog(QWidget *parent)
{
    auto &r=runtime();
    QDialog dialog(parent);
    dialog.setObjectName("NoctisModeratorExtensionDialog");
    dialog.setWindowTitle(uiText("Moderator module"));
    dialog.resize(560,370);
    dialog.setFont(parent->font());
    auto layout=new QVBoxLayout(&dialog);
    auto version=new QLabel(&dialog);
    version->setObjectName("ModeratorExtensionVersion");
    version->setTextFormat(Qt::PlainText);
    layout->addWidget(version);
    auto pages=new QTabWidget(&dialog);
    pages->setObjectName("ModeratorExtensionPages");
    for(const auto &p:r.storage.active().pages)
    {
        auto page=new QWidget(pages);
        auto column=new QVBoxLayout(page);
        auto label=new QLabel(p.body.localized(),page);
        label->setTextFormat(Qt::PlainText);label->setWordWrap(true);
        column->addWidget(label);column->addStretch();
        pages->addTab(page,p.title.localized());
    }
    if(pages->count())layout->addWidget(pages,1);
    else
    {
        pages->hide();
        auto hint=new QLabel(uiText("Module updates do not replace your rules or accounts."),&dialog);
        hint->setWordWrap(true);layout->addWidget(hint,1);
    }
    auto status=new QLabel(&dialog);
    status->setTextFormat(Qt::PlainText);status->setWordWrap(true);
    layout->addWidget(status);
    auto buttons=new QHBoxLayout;
    auto check=new QPushButton(uiText("Check for module updates"),&dialog);
    auto import=new QPushButton(uiText("Install signed package"),&dialog);
    auto close=new QPushButton(uiText("Close"),&dialog);
    buttons->addWidget(check);buttons->addWidget(import);buttons->addStretch();buttons->addWidget(close);
    layout->addLayout(buttons);
    auto refresh=[&]{
        version->setText(uiText("Module version: %1").arg(r.storage.active().version));
        status->setText(r.storage.pending() ? uiText("Update verified. Close the settings windows to apply it.") : r.status);
        check->setEnabled(!r.fetching);
    };
    QObject::connect(check,&QPushButton::clicked,&dialog,[&]{r.refresh();refresh();});
    QObject::connect(import,&QPushButton::clicked,&dialog,[&]{
        QFileDialog picker(&dialog,uiText("Install signed package"),{},QStringLiteral("JSON (*.json)"));
        picker.setOption(QFileDialog::DontUseNativeDialog);
        picker.setFileMode(QFileDialog::ExistingFile);
        if(automationdialog::run(picker)!=QDialog::Accepted)return;
        const auto file=picker.selectedFiles().value(0);
        if(file.isEmpty())return;
        r.setStatus(r.storage.offer(boundedRead(file)));refresh();
    });
    QObject::connect(close,&QPushButton::clicked,&dialog,&QDialog::reject);
    QTimer tick(&dialog);tick.setInterval(400);QObject::connect(&tick,&QTimer::timeout,&dialog,refresh);tick.start();
    installMoltorinoDialogTheme(&dialog,[&]{
        auto *t=getTheme();
        dialog.setStyleSheet(QStringLiteral("QDialog,QTabWidget::pane{background:%1;color:%2;} QLabel{color:%2;} QPushButton{background:%3;color:%2;border:1px solid %4;border-radius:5px;padding:6px 9px;} QPushButton:hover{border-color:%5;} QTabBar::tab{padding:7px 10px;color:%2;} QTabBar::tab:selected{border-bottom:2px solid %5;}")
            .arg(t->messages.backgrounds.regular.name(),t->messages.textColors.regular.name(),
                 t->messages.backgrounds.alternate.name(),t->tabs.dividerLine.name(),t->tabs.selected.line.regular.name()));
    });
    refresh();automationdialog::run(dialog);
}
} // namespace

QString Text::localized() const {return activeUiLanguage()=="ru"?ru:en;}
QString Bundle::label(const QString &key,const QString &fallback) const
{
    const auto it=labels.constFind(key);
    return it==labels.cend()?uiText(fallback):it->localized();
}
std::optional<RewardAction> Bundle::rewardAction(const QString &id) const
{
    for(const auto &action:rewardActions)if(action.id==id)return action;
    return {};
}
QByteArray ownerKey()
{
    return QByteArray::fromBase64("3GB9OcyrH3zdcuR5xZAkeoyhQgO1AhhHpsJmWnU8owM=");
}
bool validActionId(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("\\A[a-z][a-z0-9-]{0,39}\\z"));
    return re.match(id).hasMatch();
}
std::optional<Bundle> verify(const QByteArray &envelope,const QByteArray &key)
{
    if(envelope.isEmpty() || envelope.size()>MAX_BUNDLE)return {};
    const auto outer=QJsonDocument::fromJson(envelope).object();
    if(!onlyKeys(outer,{"payload","signature"}))return {};
    const auto raw=QByteArray::fromBase64(outer.value("payload").toString().toLatin1(),QByteArray::AbortOnBase64DecodingErrors);
    const auto sig=QByteArray::fromBase64(outer.value("signature").toString().toLatin1(),QByteArray::AbortOnBase64DecodingErrors);
    if(raw.isEmpty() || !signatureOk(key,raw,sig))return {};
    const auto obj=QJsonDocument::fromJson(raw).object();
    const int api=obj.value("host_api").toInt();
    if(!onlyKeys(obj,{"type","schema","host_api","revision","version","labels","pages","reward_actions","layout"}) ||
       obj.value("type").toString()!="noctis-moderator-extension" || obj.value("schema").toInt()!=1 ||
       (api!=1 && api!=2) || (api==1 && obj.contains("layout")) || !obj.value("labels").isObject() ||
       !obj.value("pages").isArray() || !obj.value("reward_actions").isArray())return {};
    Bundle b;
    b.revision=obj.value("revision").toInt(0);
    b.version=obj.value("version").toString();
    static const QRegularExpression version(QStringLiteral("\\A[0-9]{1,4}\\.[0-9]{1,4}\\.[0-9]{1,4}\\z"));
    if(b.revision<=0 || !version.match(b.version).hasMatch())return {};
    b.digest=QCryptographicHash::hash(raw,QCryptographicHash::Sha256);
    const QSet<QString> allowedLabels{"rules","options","timer","moderation","words","rewards","history","module","action_settings"};
    const auto labels=obj.value("labels").toObject();
    if(!onlyKeys(labels,allowedLabels))return {};
    for(auto it=labels.begin();it!=labels.end();++it)
    {
        auto label=text(it.value(),36);
        if(!label)return {};
        b.labels.insert(it.key(),*label);
    }
    const auto pages=obj.value("pages").toArray();
    if(pages.size()>6)return {};
    for(const auto &v:pages)
    {
        const auto page=v.toObject();
        if(!onlyKeys(page,{"title","body"}))return {};
        const auto title=text(page.value("title"),36),body=text(page.value("body"),1600);
        if(!title || !body)return {};
        b.pages.append({*title,*body});
    }
    if(obj.contains("layout"))
    {
        const auto layout=obj.value("layout").toObject();
        if(!obj.value("layout").isObject()||!onlyKeys(layout,{"reply_tabs","moderation_tabs"}))return {};
        if(layout.contains("reply_tabs"))
        {
            auto order=parseOrder(layout.value("reply_tabs"),b.replyOrder);
            if(!order)return {};
            b.replyOrder=*order;
        }
        if(layout.contains("moderation_tabs"))
        {
            auto order=parseOrder(layout.value("moderation_tabs"),b.moderationOrder);
            if(!order)return {};
            b.moderationOrder=*order;
        }
    }
    const auto actions=obj.value("reward_actions").toArray();
    if(actions.size()>12)return {};
    QSet<QString> ids;
    for(const auto &v:actions)
    {
        const auto action=v.toObject();
        if(!onlyKeys(action,{"id","title","minimum_seconds","maximum_seconds","sources","fields","program"}))return {};
        const auto id=action.value("id").toString();
        const auto title=text(action.value("title"),52);
        const int min=action.value("minimum_seconds").toInt(0),max=action.value("maximum_seconds").toInt(0);
        if(!validActionId(id) || ids.contains(id) || !title)return {};
        RewardAction a{id,*title,min,max};
        if(action.contains("program"))
        {
            if(api!=2 || !action.value("program").isObject() || !action.value("fields").isArray() ||
               !action.value("sources").isArray())return {};
            a.sources.clear();
            for(const auto source:action.value("sources").toArray())
            {
                const auto name=source.toString();
                if((name!="message" && name!="reward") || a.sources.contains(name))return {};
                a.sources.append(name);
            }
            if(a.sources.isEmpty())return {};
            const auto fields=action.value("fields").toArray();
            if(fields.size()>12)return {};
            QSet<QString> names;
            for(const auto v:fields)
            {
                const auto f=parseField(v.toObject());
                if(!f||names.contains(f->id))return {};
                names.insert(f->id);a.fields.append(*f);
            }
            a.program=action.value("program").toObject();
            if(!onlyKeys(a.program,{"operation","seconds","text"}) ||
               !a.program.contains("operation"))return {};
            int budget=96;
            for(auto it=a.program.begin();it!=a.program.end();++it)
                if(!checkExpr(it.value(),names,0,budget))return {};
        }
        else
        {
            if(min<1 || max<min || max>86400 || action.contains("fields") ||
                action.contains("sources"))return {};
        }
        ids.insert(id);b.rewardActions.append(std::move(a));
    }
    return b;
}
Store::Store(QString cache,QByteArray key):cache_(std::move(cache)),key_(std::move(key))
{
    if(!cache_.isEmpty())if(auto bundle=verify(boundedRead(cache_),key_))active_=std::move(*bundle);
}
Store::Result Store::offer(const QByteArray &envelope)
{
    const auto candidate=verify(envelope,key_);
    if(!candidate)return Result::Rejected;
    const auto &latest=pending_?*pending_:active_;
    if(candidate->revision<latest.revision ||
       (candidate->revision==latest.revision && candidate->digest!=latest.digest))return Result::Rejected;
    if(candidate->revision==latest.revision)return Result::Unchanged;
    if(!cache_.isEmpty())
    {
        QSaveFile file(cache_);
        if(!file.open(QIODevice::WriteOnly) || file.write(envelope)!=envelope.size() || !file.commit())
            return Result::Rejected;
    }
    if(uiDepth_){pending_=*candidate;return Result::Deferred;}
    active_=*candidate;return Result::Activated;
}
void Store::endUi()
{
    if(uiDepth_<=0)return;
    if(--uiDepth_==0 && pending_){active_=std::move(*pending_);pending_.reset();}
}
Store &store(){return runtime().storage;}
const Bundle &active(){return store().active();}
void start()
{
    auto &r=runtime();
    if(!r.timer.isActive() && !getApp()->isTest() && !getApp()->getArgs().dontSaveSettings &&
       !getApp()->getArgs().safeMode && !getApp()->getArgs().uiSnapshot)
    {
        r.timer.start();r.refresh();
    }
}
UiLease::UiLease():store_(&store()){store_->beginUi();start();}
UiLease::~UiLease(){store_->endUi();}
QPushButton *moduleButton(QWidget *parent)
{
    auto *button=new QPushButton(active().label("module","Module"),parent);
    button->setObjectName("ModeratorExtensionButton");
    QObject::connect(button,&QPushButton::clicked,parent,[parent]{openModuleDialog(parent);});
    return button;
}
std::optional<int> rewardSeconds(const QString &id)
{
    auto action=active().rewardAction(id);
    if(!action || !action->program.isEmpty())return {};
    return QRandomGenerator::global()->bounded(action->minimumSeconds,action->maximumSeconds+1);
}
bool claimReplyEvent(const QString &room,const QString &messageId)
{
    if(room.isEmpty() || messageId.isEmpty())return true; // legacy/synthetic chat route
    static QSet<QString> claimed;
    static QQueue<QString> order;
    const auto key=room+u'/'+messageId;
    if(claimed.contains(key))return false;
    claimed.insert(key);order.enqueue(key);
    if(order.size()>8192)claimed.remove(order.dequeue());
    return true;
}
std::optional<Decision> decide(const RewardAction &action,const QJsonObject &parameters,
                               const QJsonObject &event)
{
    if(!action.sources.contains(event.value("source").toString()))return {};
    if(action.program.isEmpty())
    {
        if(action.minimumSeconds<1 || action.maximumSeconds<action.minimumSeconds || action.maximumSeconds>86400)return {};
        return Decision{Operation::Timeout,QRandomGenerator::global()->bounded(action.minimumSeconds,action.maximumSeconds+1),{}};
    }
    QJsonObject values;
    for(const auto &f:action.fields)
    {
        const auto v=parameters.contains(f.id)?parameters.value(f.id):f.initial;
        if(!fieldValue(f,v))return {};
        values.insert(f.id,v);
    }
    int budget=96;
    const auto type=eval(action.program.value("operation"),values,event,0,budget);
    if(!type || !type->isString())return {};
    const auto kind=type->toString();
    if(kind=="skip")return Decision{Operation::Skip};
    if(kind=="untimeout")return Decision{Operation::Untimeout};
    if(kind=="timeout")
    {
        const auto duration=eval(action.program.value("seconds"),values,event,0,budget);
        if(!duration || !integer(*duration) || duration->toInt()<1 || duration->toInt()>86400)return {};
        return Decision{Operation::Timeout,duration->toInt(),{}};
    }
    if(kind=="reply")
    {
        const auto message=eval(action.program.value("text"),values,event,0,budget);
        if(!message || !message->isString())return {};
        const auto plain=message->toString().trimmed();
        static const QRegularExpression control(QStringLiteral("[\\x00-\\x1f\\x7f]"));
        if(plain.isEmpty() || plain.size()>500 || plain.startsWith(u'/') ||
           plain.startsWith(u'.') || plain.contains(control))return {};
        return Decision{Operation::Reply,0,plain};
    }
    return {};
}
std::optional<QJsonObject> editParameters(QWidget *parent,const RewardAction &action,
                                          const QJsonObject &current)
{
    if(action.fields.isEmpty())return current;
    QDialog dialog(parent);
    dialog.setObjectName("NoctisModuleActionParameters");
    dialog.setWindowTitle(action.title.localized());
    dialog.setFont(parent->font());
    dialog.resize(520,200);
    auto *layout=new QVBoxLayout(&dialog);
    auto *form=new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    layout->addLayout(form);
    QList<QPair<QString,QWidget *>> widgets;
    for(const auto &field:action.fields)
    {
        auto value=current.value(field.id);
        if(!fieldValue(field,value))value=field.initial;
        QWidget *input=nullptr;
        if(field.type=="integer")
        {
            auto *spin=new QSpinBox(&dialog);
            spin->setRange(field.minimum,field.maximum);
            spin->setValue(value.toInt());input=spin;
        }
        else if(field.type=="boolean")
        {
            auto *check=new QCheckBox(&dialog);check->setChecked(value.toBool());input=check;
        }
        else if(field.type=="choice")
        {
            auto *combo=new QComboBox(&dialog);
            for(const auto &choice:field.choices)combo->addItem(choice.title.localized(),choice.id);
            combo->setCurrentIndex(combo->findData(value.toString()));input=combo;
        }
        else
        {
            auto *line=new QLineEdit(value.toString(),&dialog);line->setMaxLength(field.maximum);input=line;
        }
        input->setObjectName(QStringLiteral("ModuleParameter_")+field.id);
        auto *label=new QLabel(field.title.localized(),&dialog);
        label->setWordWrap(true);label->setTextFormat(Qt::PlainText);label->setBuddy(input);
        form->addRow(label,input);
        widgets.append({field.id,input});
    }
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(uiText("OK"));
    buttons->button(QDialogButtonBox::Cancel)->setText(uiText("Cancel"));
    QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
    QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    layout->addWidget(buttons);
    installMoltorinoDialogTheme(&dialog,[&]{
        auto *t=getTheme();
        dialog.setStyleSheet(QStringLiteral("QDialog{background:%1;color:%2;} QLabel{color:%2;} QPushButton,QSpinBox,QComboBox,QLineEdit{background:%3;color:%2;border:1px solid %4;border-radius:5px;padding:5px 8px;} QPushButton:hover,QLineEdit:focus{border-color:%5;}")
            .arg(t->messages.backgrounds.regular.name(),t->messages.textColors.regular.name(),
                 t->messages.backgrounds.alternate.name(),t->tabs.dividerLine.name(),t->tabs.selected.line.regular.name()));
    });
    if(automationdialog::run(dialog)!=QDialog::Accepted)return {};
    auto result=current;
    for(const auto &[id,input]:widgets)
    {
        if(auto *spin=qobject_cast<QSpinBox *>(input))result[id]=spin->value();
        else if(auto *check=qobject_cast<QCheckBox *>(input))result[id]=check->isChecked();
        else if(auto *combo=qobject_cast<QComboBox *>(input))result[id]=combo->currentData().toString();
        else if(auto *line=qobject_cast<QLineEdit *>(input))result[id]=line->text();
    }
    return result;
}
} // namespace chatterino::moderatorextension
