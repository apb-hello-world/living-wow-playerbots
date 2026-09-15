// Supplied rows exercise the actual custody predicate against MariaDB. This is
// database-boundary proof, not a native send, customer receipt or payment.
static void CommissionMailDatabase(Connection& db) {
    Task task;task.id=task.root="d1ca4b79-94ed-49fb-8c06-f54fa826c131";
    task.actor=9981;task.revision=7;
    CommissionMailQuote quote{"lwc-987",9981,9982,9981001,2454,1,1,500,30,120,120,23,123456789};
    const std::string operation="2f5efaed-b5d7-4f02-8743-1af0ea9e1604";
    AuctionMail sent;sent.id=90031;sent.sender=quote.sender;sent.receiver=quote.receiver;
    sent.cod=120;sent.itemGuid=quote.item;sent.itemEntry=quote.entry;sent.quantity=1;sent.attachments=1;
    sent.deliveredAt=1120;sent.expiresAt=200000;sent.subject=CommissionMailSubject(operation);
    assert(db.Execute("ALTER TABLE mail ADD COLUMN subject VARCHAR(255) NOT NULL DEFAULT ''"));
    assert(db.Execute("INSERT INTO mail(id,messageType,sender,receiver,money,cod,deliver_time,expire_time,subject) VALUES(90031,0,9981,9982,0,120,1120,200000,"+SqlValue(sent.subject)+')'));
    assert(db.Execute("INSERT INTO characters(guid,race,money) VALUES(9981,5,470),(9982,5,1000)"));
    assert(db.Execute("INSERT INTO mail_items VALUES(90031,9981001,2454,9982)"));
    assert(db.Execute("INSERT INTO item_instance(guid,itemEntry,owner_guid,count) VALUES(9981001,2454,9982,1)"));
    const auto proof=CommissionMailSentProof(task,quote,sent,operation);
    assert(db.Scalar(proof)==task.id);
    const std::vector<std::string> mismatches={
        "UPDATE mail SET receiver=9983 WHERE id=90031",
        "UPDATE mail SET sender=9983 WHERE id=90031",
        "UPDATE mail SET cod=0 WHERE id=90031",
        "UPDATE mail SET money=120 WHERE id=90031",
        "UPDATE mail SET messageType=2 WHERE id=90031",
        "UPDATE mail SET subject='unrelated parcel' WHERE id=90031",
        "UPDATE mail SET deliver_time=1121 WHERE id=90031",
        "UPDATE mail SET expire_time=200001 WHERE id=90031",
        "UPDATE mail_items SET receiver=9983 WHERE mail_id=90031",
        "UPDATE item_instance SET owner_guid=9981 WHERE guid=9981001",
        "UPDATE item_instance SET count=2 WHERE guid=9981001",
        "UPDATE item_instance SET itemEntry=2455 WHERE guid=9981001",
        "UPDATE characters SET money=500 WHERE guid=9981",
        "INSERT INTO mail_items VALUES(90031,9981002,2454,9982)",
        "INSERT INTO mail_items VALUES(90032,9981001,2454,9982)",
        "INSERT INTO guild_bank_item VALUES(31,9981001,2454)",
        "INSERT INTO character_inventory(guid,bag,slot,item,item_template) VALUES(9982,0,23,9981001,2454)"
    };
    for(const auto& mutation:mismatches) {
        assert(db.Execute("START TRANSACTION"));assert(db.Execute(mutation));
        assert(db.Scalar(proof).empty());assert(db.Execute("ROLLBACK"));
        assert(db.Scalar(proof)==task.id);
    }
    // Mailing never spends the customer's fee or credits it to the crafter.
    assert(db.Scalar("SELECT money FROM characters WHERE guid=9982")=="1000");
    assert(db.Scalar("SELECT money FROM characters WHERE guid=9981")=="470");
}
