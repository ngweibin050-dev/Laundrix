import { collection, getDocs } from "firebase/firestore";
import { db } from "@/services/firebase";

export const dashboardDataSource = {
  fetchAll() {
    return getDocs(collection(db, "machines"));
  },
};
